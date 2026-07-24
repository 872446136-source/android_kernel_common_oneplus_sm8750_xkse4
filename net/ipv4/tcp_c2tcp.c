// SPDX-License-Identifier: GPL-2.0-only
/*
 * TCP C2TCP: Cellular Controlled Delay TCP over CUBIC.
 *
 * The delay-control state machine in this file is a clean-room
 * implementation based on:
 *
 *   Soheil Abbasloo, Tong Li, Yang Xu, and H. Jonathan Chao,
 *   "Cellular Controlled Delay TCP (C2TCP)", IFIP Networking 2018.
 *
 * The authors' public artifact is available at:
 *
 *   https://github.com/soheil-ab/C2TCP-IFIP
 *
 * That repository does not state an explicit source-code license for its
 * C2TCP additions.  No C2TCP source from the artifact is copied here; the
 * delay controller below is implemented from the paper's state machine.
 *
 * The loss-based controller is derived from the current in-tree CUBIC
 * implementation and retains its GPL-2.0-only licensing and semantics:
 *
 * TCP CUBIC: Binary Increase Congestion control for TCP v2.3
 * Home page:
 *      http://netsrv.csc.ncsu.edu/twiki/bin/view/Main/BIC
 * This is from the implementation of CUBIC TCP in
 * Sangtae Ha, Injong Rhee and Lisong Xu,
 *  "CUBIC: A New TCP-Friendly High-Speed TCP Variant"
 *  in ACM SIGOPS Operating System Review, July 2008.
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/cubic_a_new_tcp_2008.pdf
 *
 * CUBIC integrates a new slow start algorithm, called HyStart.
 * The details of HyStart are presented in
 *  Sangtae Ha and Injong Rhee,
 *  "Taming the Elephants: New TCP Slow Start", NCSU TechReport 2008.
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/hystart_techreport_2008.pdf
 *
 * All testing results are available from:
 * http://netsrv.csc.ncsu.edu/wiki/index.php/TCP_Testing
 *
 * Unless CUBIC is enabled and congestion window is large
 * this behaves the same as the original Reno.
 */

#include <linux/mm.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <net/codel.h>
#include <net/tcp.h>

#define C2TCP_BETA_SCALE	1024	/* Scale factor beta calculation:
					 * max_cwnd = snd_cwnd * beta
					 */
#define C2TCP_HZ		10	/* CUBIC HZ 2^10 = 1024 */

/* Two methods of hybrid slow start. */
#define C2TCP_HYSTART_ACK_TRAIN	0x1
#define C2TCP_HYSTART_DELAY	0x2

/* Number of delay samples for detecting the increase of delay. */
#define C2TCP_HYSTART_MIN_SAMPLES	8
#define C2TCP_HYSTART_DELAY_MIN		4000U	/* 4 ms */
#define C2TCP_HYSTART_DELAY_MAX		16000U	/* 16 ms */
#define C2TCP_HYSTART_DELAY_THRESH(x)					\
	clamp(x, C2TCP_HYSTART_DELAY_MIN, C2TCP_HYSTART_DELAY_MAX)

/* IFIP C2TCP v1 parameters.  They intentionally are not module parameters:
 * every connection uses the reference target, interval, and alpha.
 */
#define C2TCP_TARGET_MS		100U
#define C2TCP_TARGET_US		(C2TCP_TARGET_MS * USEC_PER_MSEC)
#define C2TCP_INTERVAL_MS	100U
#define C2TCP_ALPHA		0U

static int c2tcp_fast_convergence __read_mostly = 1;
static int c2tcp_beta __read_mostly = 717;
static int c2tcp_initial_ssthresh __read_mostly;
static int c2tcp_bic_scale __read_mostly = 41;
static int c2tcp_tcp_friendliness __read_mostly = 1;

static int c2tcp_hystart __read_mostly = 1;
static int c2tcp_hystart_detect __read_mostly =
	C2TCP_HYSTART_ACK_TRAIN | C2TCP_HYSTART_DELAY;
static int c2tcp_hystart_low_window __read_mostly = 16;
static int c2tcp_hystart_ack_delta_us __read_mostly = 2000;

static u32 c2tcp_cube_rtt_scale __read_mostly;
static u32 c2tcp_beta_scale __read_mostly;
static u64 c2tcp_cube_factor __read_mostly;

/* Parameters used for precomputing scale factors are read-only. */
module_param_named(fast_convergence, c2tcp_fast_convergence, int, 0644);
MODULE_PARM_DESC(fast_convergence, "turn on/off fast convergence");
module_param_named(beta, c2tcp_beta, int, 0644);
MODULE_PARM_DESC(beta, "beta for multiplicative increase");
module_param_named(initial_ssthresh, c2tcp_initial_ssthresh, int, 0644);
MODULE_PARM_DESC(initial_ssthresh, "initial value of slow start threshold");
module_param_named(bic_scale, c2tcp_bic_scale, int, 0444);
MODULE_PARM_DESC(bic_scale,
		 "scale (scaled by 1024) value for cubic function");
module_param_named(tcp_friendliness, c2tcp_tcp_friendliness, int, 0644);
MODULE_PARM_DESC(tcp_friendliness, "turn on/off TCP friendliness");
module_param_named(hystart, c2tcp_hystart, int, 0644);
MODULE_PARM_DESC(hystart, "turn on/off hybrid slow start algorithm");
module_param_named(hystart_detect, c2tcp_hystart_detect, int, 0644);
MODULE_PARM_DESC(hystart_detect,
		 "hybrid slow start detection: 1 packet-train, 2 delay, 3 both");
module_param_named(hystart_low_window, c2tcp_hystart_low_window, int, 0644);
MODULE_PARM_DESC(hystart_low_window, "lower bound cwnd for hybrid slow start");
module_param_named(hystart_ack_delta_us, c2tcp_hystart_ack_delta_us,
		   int, 0644);
MODULE_PARM_DESC(hystart_ack_delta_us,
		 "ACK spacing indicating an ACK train (usecs)");

struct c2tcp_ca {
	/* CUBIC state. */
	u32	cnt;
	u32	last_max_cwnd;
	u32	last_cwnd;
	u32	last_time;
	u32	bic_origin_point;
	u32	bic_K;
	u32	delay_min;
	u32	epoch_start;
	u32	ack_cnt;
	u32	tcp_cwnd;
	u16	unused;
	u8	sample_cnt;
	u8	found;
	u32	round_start;
	u32	end_seq;
	u32	last_ack;
	u32	curr_rtt;

	/* Per-connection C2TCP observation state. */
	codel_time_t	first_above_time;
	codel_time_t	next_time;
	u32		bad_count;
	u16		rec_inv_sqrt;
	u16		pad;
};

static inline void c2tcp_reset(struct c2tcp_ca *ca)
{
	memset(ca, 0, offsetof(struct c2tcp_ca, unused));
	ca->found = 0;
}

static inline void c2tcp_observation_reset(struct c2tcp_ca *ca)
{
	ca->first_above_time = 0;
	ca->next_time = 0;
	ca->bad_count = 1;
	ca->rec_inv_sqrt = 0;
}

static inline u32 c2tcp_clock_us(const struct sock *sk)
{
	return tcp_sk(sk)->tcp_mstamp;
}

static inline void c2tcp_hystart_reset(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct c2tcp_ca *ca = inet_csk_ca(sk);

	ca->round_start = c2tcp_clock_us(sk);
	ca->last_ack = ca->round_start;
	ca->end_seq = tp->snd_nxt;
	ca->curr_rtt = ~0U;
	ca->sample_cnt = 0;
}

__bpf_kfunc static void c2tcp_init(struct sock *sk)
{
	struct c2tcp_ca *ca = inet_csk_ca(sk);

	c2tcp_reset(ca);
	c2tcp_observation_reset(ca);

	if (c2tcp_hystart)
		c2tcp_hystart_reset(sk);

	if (!c2tcp_hystart && c2tcp_initial_ssthresh)
		tcp_sk(sk)->snd_ssthresh = c2tcp_initial_ssthresh;
}

__bpf_kfunc static void c2tcp_cwnd_event(struct sock *sk,
					 enum tcp_ca_event event)
{
	struct c2tcp_ca *ca = inet_csk_ca(sk);

	if (event == CA_EVENT_TX_START) {
		u32 now = tcp_jiffies32;
		s32 delta;

		delta = now - tcp_sk(sk)->lsndtime;

		/* We were application limited (idle) for a while.  Shift
		 * epoch_start to keep cwnd growth on the CUBIC curve.
		 */
		if (ca->epoch_start && delta > 0) {
			ca->epoch_start += delta;
			if (after(ca->epoch_start, now))
				ca->epoch_start = now;
		}
		c2tcp_observation_reset(ca);
		return;
	}

	if (event == CA_EVENT_CWND_RESTART || event == CA_EVENT_LOSS)
		c2tcp_observation_reset(ca);
}

/* Calculate the cubic root of x using a table lookup followed by one
 * Newton-Raphson iteration.  Average error is approximately 0.195%.
 */
static u32 c2tcp_cubic_root(u64 a)
{
	u32 x, b, shift;
	/* cbrt(x) MSB values for x MSB values in [0..63].
	 * Precomputed then refined by hand - Willy Tarreau.
	 */
	static const u8 c2tcp_cubic_root_table[] = {
		/* 0x00 */    0,   54,   54,   54,  118,  118,  118,  118,
		/* 0x08 */  123,  129,  134,  138,  143,  147,  151,  156,
		/* 0x10 */  157,  161,  164,  168,  170,  173,  176,  179,
		/* 0x18 */  181,  185,  187,  190,  192,  194,  197,  199,
		/* 0x20 */  200,  202,  204,  206,  209,  211,  213,  215,
		/* 0x28 */  217,  219,  221,  222,  224,  225,  227,  229,
		/* 0x30 */  231,  232,  234,  236,  237,  239,  240,  242,
		/* 0x38 */  244,  245,  246,  248,  250,  251,  252,  254,
	};

	b = fls64(a);
	if (b < 7)
		return ((u32)c2tcp_cubic_root_table[(u32)a] + 35) >> 6;

	b = ((b * 84) >> 8) - 1;
	shift = a >> (b * 3);

	x = ((u32)(((u32)c2tcp_cubic_root_table[shift] + 10) << b)) >> 6;

	/* Newton-Raphson iteration:
	 *
	 *                  2
	 * x    = (2 * x + a / x ) / 3
	 *  k+1        k         k
	 */
	x = 2 * x + (u32)div64_u64(a, (u64)x * (u64)(x - 1));
	x = (x * 341) >> 10;
	return x;
}

/* Compute the CUBIC congestion window increment. */
static inline void c2tcp_update(struct c2tcp_ca *ca, u32 cwnd, u32 acked)
{
	u32 delta, bic_target, max_cnt;
	u64 offs, t;

	ca->ack_cnt += acked;

	if (ca->last_cwnd == cwnd &&
	    (s32)(tcp_jiffies32 - ca->last_time) <= HZ / 32)
		return;

	if (ca->epoch_start && tcp_jiffies32 == ca->last_time)
		goto tcp_friendliness;

	ca->last_cwnd = cwnd;
	ca->last_time = tcp_jiffies32;

	if (ca->epoch_start == 0) {
		ca->epoch_start = tcp_jiffies32;
		ca->ack_cnt = acked;
		ca->tcp_cwnd = cwnd;

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
		} else {
			ca->bic_K =
				c2tcp_cubic_root(c2tcp_cube_factor *
						 (ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
		}
	}

	t = (s32)(tcp_jiffies32 - ca->epoch_start);
	t += usecs_to_jiffies(ca->delay_min);
	t <<= C2TCP_HZ;
	do_div(t, HZ);

	if (t < ca->bic_K)
		offs = ca->bic_K - t;
	else
		offs = t - ca->bic_K;

	delta = (c2tcp_cube_rtt_scale * offs * offs * offs) >>
		(10 + 3 * C2TCP_HZ);
	if (t < ca->bic_K)
		bic_target = ca->bic_origin_point - delta;
	else
		bic_target = ca->bic_origin_point + delta;

	if (bic_target > cwnd)
		ca->cnt = cwnd / (bic_target - cwnd);
	else
		ca->cnt = 100 * cwnd;

	if (ca->last_max_cwnd == 0 && ca->cnt > 20)
		ca->cnt = 20;

tcp_friendliness:
	if (c2tcp_tcp_friendliness) {
		u32 scale = c2tcp_beta_scale;

		delta = (cwnd * scale) >> 3;
		while (ca->ack_cnt > delta) {
			ca->ack_cnt -= delta;
			ca->tcp_cwnd++;
		}

		if (ca->tcp_cwnd > cwnd) {
			delta = ca->tcp_cwnd - cwnd;
			max_cnt = cwnd / delta;
			if (ca->cnt > max_cnt)
				ca->cnt = max_cnt;
		}
	}

	/* CUBIC allows at most one packet of growth per two ACKed packets. */
	ca->cnt = max(ca->cnt, 2U);
}

__bpf_kfunc static void c2tcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct c2tcp_ca *ca = inet_csk_ca(sk);

	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tcp_in_slow_start(tp)) {
		acked = tcp_slow_start(tp, acked);
		if (!acked)
			return;
	}
	c2tcp_update(ca, tcp_snd_cwnd(tp), acked);
	tcp_cong_avoid_ai(tp, ca->cnt, acked);
}

__bpf_kfunc static u32 c2tcp_recalc_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct c2tcp_ca *ca = inet_csk_ca(sk);

	ca->epoch_start = 0;

	if (tcp_snd_cwnd(tp) < ca->last_max_cwnd &&
	    c2tcp_fast_convergence)
		ca->last_max_cwnd =
			(tcp_snd_cwnd(tp) * (C2TCP_BETA_SCALE + c2tcp_beta)) /
			(2 * C2TCP_BETA_SCALE);
	else
		ca->last_max_cwnd = tcp_snd_cwnd(tp);

	return max((tcp_snd_cwnd(tp) * c2tcp_beta) /
		   C2TCP_BETA_SCALE, 2U);
}

/* Newton step for the Q0.32 reciprocal square-root used by CoDel's
 * interval/sqrt(count) control law.
 */
static void c2tcp_newton_step(struct c2tcp_ca *ca)
{
	u32 invsqrt = (u32)ca->rec_inv_sqrt << REC_INV_SQRT_SHIFT;
	u32 invsqrt2 = ((u64)invsqrt * invsqrt) >> 32;
	u64 val = 3ULL << 32;
	u64 product = (u64)ca->bad_count * invsqrt2;

	/* A corrupt or saturated state must not underflow the iteration. */
	if (unlikely(product >= val)) {
		ca->rec_inv_sqrt = 1;
		return;
	}

	val -= product;
	val >>= 2;
	val = (val * invsqrt) >> (32 - 2 + 1);
	ca->rec_inv_sqrt =
		max_t(u16, (u16)(val >> REC_INV_SQRT_SHIFT), 1U);
}

static codel_time_t c2tcp_add_delta(codel_time_t now, codel_time_t delta)
{
	codel_time_t deadline;

	/* Keep zero available as the "not observing" sentinel. */
	deadline = now + max_t(codel_time_t, delta, 1U);
	return deadline ? deadline : 1U;
}

static codel_time_t c2tcp_control_law(codel_time_t now,
				      codel_time_t interval,
				      u16 rec_inv_sqrt)
{
	u32 reciprocal;
	codel_time_t delta;

	reciprocal = (u32)max_t(u16, rec_inv_sqrt, 1U) <<
		     REC_INV_SQRT_SHIFT;
	delta = reciprocal_scale(interval, reciprocal);
	return c2tcp_add_delta(now, delta);
}

static void c2tcp_start_observation(struct c2tcp_ca *ca,
				    codel_time_t now)
{
	codel_time_t interval = MS2TIME(C2TCP_INTERVAL_MS);

	ca->first_above_time = c2tcp_add_delta(now, interval);
	ca->next_time = ca->first_above_time;
	ca->bad_count = 1;
	ca->rec_inv_sqrt = ~0U >> REC_INV_SQRT_SHIFT;
	c2tcp_newton_step(ca);
}

static void c2tcp_add_good_credit(struct sock *sk, u32 rtt_us)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 rtt_ms = max_t(u32, rtt_us / USEC_PER_MSEC, 1U);
	u32 gain = C2TCP_ALPHA + C2TCP_TARGET_MS / rtt_ms;
	u32 cwnd = max_t(u32, tcp_snd_cwnd(tp), 1U);
	u64 credit;

	/* The IFIP implementation applies one gain unit per RTT callback,
	 * independent of sample->pkts_acked.  Preserve that delayed-ACK
	 * sensitivity while using wide arithmetic to avoid counter overflow.
	 */
	if (!gain)
		return;

	credit = (u64)tp->snd_cwnd_cnt + gain;
	if (credit >= cwnd) {
		tp->snd_cwnd_cnt = 0;
		if (tcp_snd_cwnd(tp) < tp->snd_cwnd_clamp)
			tcp_snd_cwnd_set(tp, tcp_snd_cwnd(tp) + 1);
	} else {
		tp->snd_cwnd_cnt = (u32)credit;
	}
}

static void c2tcp_delay_control(struct sock *sk, u32 rtt_us)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct c2tcp_ca *ca = inet_csk_ca(sk);
	codel_time_t interval = MS2TIME(C2TCP_INTERVAL_MS);
	codel_time_t now;

	if (rtt_us <= C2TCP_TARGET_US) {
		c2tcp_observation_reset(ca);
		c2tcp_add_good_credit(sk, rtt_us);
		return;
	}

	now = codel_get_time();
	if (!ca->first_above_time) {
		c2tcp_start_observation(ca, now);
		return;
	}

	/* Normal: at least one interval has not yet remained above target. */
	if (!codel_time_after(now, ca->next_time))
		return;

	/* Bad: preserve CUBIC's loss-state bookkeeping and beta ssthresh,
	 * then force slow start from one packet as specified by C2TCP.
	 */
	ca->next_time =
		c2tcp_control_law(now, interval, ca->rec_inv_sqrt);
	if (ca->bad_count != U32_MAX)
		ca->bad_count++;
	c2tcp_newton_step(ca);

	tp->prior_ssthresh = tcp_current_ssthresh(sk);
	tp->snd_ssthresh = c2tcp_recalc_ssthresh(sk);
	tcp_snd_cwnd_set(tp, 1U);
	tp->snd_cwnd_cnt = 0;
}

__bpf_kfunc static void c2tcp_state(struct sock *sk, u8 new_state)
{
	if (new_state == TCP_CA_Loss) {
		struct c2tcp_ca *ca = inet_csk_ca(sk);

		c2tcp_reset(ca);
		c2tcp_hystart_reset(sk);
		c2tcp_observation_reset(ca);
	}
}

/* Account for TSO/GRO delays.  Otherwise short RTT flows could get too
 * small ssthresh because early slow start uses smaller TSO packets.
 */
static u32 c2tcp_hystart_ack_delay(const struct sock *sk)
{
	unsigned long rate;

	rate = READ_ONCE(sk->sk_pacing_rate);
	if (!rate)
		return 0;
	return min_t(u64, USEC_PER_MSEC,
		     div64_ul((u64)sk->sk_gso_max_size * 4 * USEC_PER_SEC,
			      rate));
}

static void c2tcp_hystart_update(struct sock *sk, u32 delay)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct c2tcp_ca *ca = inet_csk_ca(sk);
	u32 threshold;

	if (after(tp->snd_una, ca->end_seq))
		c2tcp_hystart_reset(sk);

	if (tcp_snd_cwnd(tp) < c2tcp_hystart_low_window)
		return;

	if (c2tcp_hystart_detect & C2TCP_HYSTART_ACK_TRAIN) {
		u32 now = c2tcp_clock_us(sk);

		if ((s32)(now - ca->last_ack) <=
		    c2tcp_hystart_ack_delta_us) {
			ca->last_ack = now;
			threshold = ca->delay_min +
				    c2tcp_hystart_ack_delay(sk);

			if (sk->sk_pacing_status == SK_PACING_NONE)
				threshold >>= 1;

			if ((s32)(now - ca->round_start) > threshold) {
				ca->found = 1;
				pr_debug("c2tcp_hystart_ack_train (%u > %u) delay_min %u (+ ack_delay %u) cwnd %u\n",
					 now - ca->round_start, threshold,
					 ca->delay_min,
					 c2tcp_hystart_ack_delay(sk),
					 tcp_snd_cwnd(tp));
				NET_INC_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTTRAINDETECT);
				NET_ADD_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTTRAINCWND,
					      tcp_snd_cwnd(tp));
				tp->snd_ssthresh = tcp_snd_cwnd(tp);
			}
		}
	}

	if (c2tcp_hystart_detect & C2TCP_HYSTART_DELAY) {
		if (ca->curr_rtt > delay)
			ca->curr_rtt = delay;
		if (ca->sample_cnt < C2TCP_HYSTART_MIN_SAMPLES) {
			ca->sample_cnt++;
		} else if (ca->curr_rtt >
			   ca->delay_min +
			   C2TCP_HYSTART_DELAY_THRESH(ca->delay_min >> 3)) {
			ca->found = 1;
			NET_INC_STATS(sock_net(sk),
				      LINUX_MIB_TCPHYSTARTDELAYDETECT);
			NET_ADD_STATS(sock_net(sk),
				      LINUX_MIB_TCPHYSTARTDELAYCWND,
				      tcp_snd_cwnd(tp));
			tp->snd_ssthresh = tcp_snd_cwnd(tp);
		}
	}
}

__bpf_kfunc static void c2tcp_acked(struct sock *sk,
				    const struct ack_sample *sample)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct c2tcp_ca *ca = inet_csk_ca(sk);
	u32 delay;

	/* Some callbacks are for duplicate ACKs without an RTT sample. */
	if (sample->rtt_us < 0)
		return;

	/* Preserve CUBIC's post-recovery sample filtering. */
	if (ca->epoch_start &&
	    (s32)(tcp_jiffies32 - ca->epoch_start) < HZ)
		return;

	delay = sample->rtt_us;
	if (delay == 0)
		delay = 1;

	if (ca->delay_min == 0 || ca->delay_min > delay)
		ca->delay_min = delay;

	if (!ca->found && tcp_in_slow_start(tp) && c2tcp_hystart)
		c2tcp_hystart_update(sk, delay);

	c2tcp_delay_control(sk, delay);
}

static struct tcp_congestion_ops c2tcp __read_mostly = {
	.init		= c2tcp_init,
	.ssthresh	= c2tcp_recalc_ssthresh,
	.cong_avoid	= c2tcp_cong_avoid,
	.set_state	= c2tcp_state,
	.undo_cwnd	= tcp_reno_undo_cwnd,
	.cwnd_event	= c2tcp_cwnd_event,
	.pkts_acked	= c2tcp_acked,
	.owner		= THIS_MODULE,
	.name		= "c2tcp",
};

BTF_SET8_START(c2tcp_check_kfunc_ids)
#ifdef CONFIG_X86
#ifdef CONFIG_DYNAMIC_FTRACE
BTF_ID_FLAGS(func, c2tcp_init)
BTF_ID_FLAGS(func, c2tcp_recalc_ssthresh)
BTF_ID_FLAGS(func, c2tcp_cong_avoid)
BTF_ID_FLAGS(func, c2tcp_state)
BTF_ID_FLAGS(func, c2tcp_cwnd_event)
BTF_ID_FLAGS(func, c2tcp_acked)
#endif
#endif
BTF_SET8_END(c2tcp_check_kfunc_ids)

static const struct btf_kfunc_id_set c2tcp_kfunc_set = {
	.owner	= THIS_MODULE,
	.set	= &c2tcp_check_kfunc_ids,
};

static int __init c2tcp_register(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct c2tcp_ca) > ICSK_CA_PRIV_SIZE);
	if (c2tcp_beta < 0 || c2tcp_beta >= C2TCP_BETA_SCALE ||
	    c2tcp_bic_scale <= 0 || c2tcp_bic_scale > INT_MAX / 10)
		return -EINVAL;

	c2tcp_beta_scale =
		8 * (C2TCP_BETA_SCALE + c2tcp_beta) / 3 /
		(C2TCP_BETA_SCALE - c2tcp_beta);

	c2tcp_cube_rtt_scale = c2tcp_bic_scale * 10;

	c2tcp_cube_factor = 1ULL << (10 + 3 * C2TCP_HZ);
	do_div(c2tcp_cube_factor, c2tcp_bic_scale * 10);

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					&c2tcp_kfunc_set);
	if (ret < 0)
		return ret;

	return tcp_register_congestion_control(&c2tcp);
}

static void __exit c2tcp_unregister(void)
{
	tcp_unregister_congestion_control(&c2tcp);
}

module_init(c2tcp_register);
module_exit(c2tcp_unregister);

MODULE_AUTHOR("Sangtae Ha");
MODULE_AUTHOR("Stephen Hemminger");
MODULE_AUTHOR("Soheil Abbasloo");
MODULE_AUTHOR("Tong Li");
MODULE_AUTHOR("Yang Xu");
MODULE_AUTHOR("H. Jonathan Chao");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("C2TCP over an independent CUBIC controller");
MODULE_VERSION("1.0");
