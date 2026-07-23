// SPDX-License-Identifier: GPL-2.0-only
/*
 * TCP ROCCET: RTT-Oriented CUBIC Congestion Control Extension
 *
 * ROCCET extends CUBIC with relative RTT and ACK-rate observations for
 * cellular and other deep-buffered networks.  Its LAUNCH phase replaces
 * HyStart, while ORBITER retains CUBIC's congestion avoidance window law.
 *
 * This implementation is based on the ROCCET v3 net-next submission and:
 *
 *   Lukas Prause and Mark Akselrod,
 *   "TCP ROCCET: An RTT-Oriented CUBIC Congestion Control Extension for
 *   5G and Beyond Networks", WONS 2026.
 *
 * The CUBIC window controller is derived from the in-tree tcp_cubic.c.
 */

#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <net/tcp.h>

#define ROCCET_BETA_SCALE		1024U
#define ROCCET_HZ			10U

#define ROCCET_ALPHA_PERCENT		20U
#define ROCCET_SRTT_THRESHOLD		100U
#define ROCCET_ACK_RATE_THRESHOLD	10U
#define ROCCET_ACK_RATE_INTERVAL_US	(100U * USEC_PER_MSEC)
#define ROCCET_ACK_RATE_IDLE_US		(2U * USEC_PER_SEC)
#define ROCCET_EVAL_RTT_COUNT		5U
#define ROCCET_DRAIN_INTERVAL_US	(100U * USEC_PER_MSEC)

#define ROCCET_BETA			717U
#define ROCCET_BIC_SCALE		41U

struct roccet_ack_rate {
	u32	last_rate;
	u32	curr_rate;
	u32	acked;
	u32	interval_start_us;
};

struct roccet_ca {
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

	/* ROCCET observation state. */
	u32	curr_rtt;
	u32	min_rtt;
	u32	curr_srrtt;
	u32	last_rtt;
	u32	next_srrtt_check_us;
	u32	last_event_us;
	struct roccet_ack_rate ack_rate;
	u8	ack_rate_samples;
};

static bool fast_convergence __read_mostly = true;
static bool tcp_friendliness __read_mostly = true;
static u32 cube_rtt_scale __read_mostly;
static u32 beta_scale __read_mostly;
static u64 cube_factor __read_mostly;

static inline u32 roccet_clock_us(const struct sock *sk)
{
	return tcp_sk(sk)->tcp_mstamp;
}

static inline bool roccet_time_after_eq(u32 now, u32 deadline)
{
	return (s32)(now - deadline) >= 0;
}

static inline u32 roccet_eval_interval_us(u32 rtt_us)
{
	u64 interval = (u64)rtt_us * ROCCET_EVAL_RTT_COUNT;

	return min_t(u64, interval, S32_MAX);
}

static inline u32 roccet_saturating_add(u32 value, u32 addend)
{
	return min_t(u64, (u64)value + addend, U32_MAX);
}

static void roccettcp_reset(struct roccet_ca *ca)
{
	memset(ca, 0, sizeof(*ca));
}

static void roccettcp_init(struct sock *sk)
{
	roccettcp_reset(inet_csk_ca(sk));
}

static void roccettcp_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	struct roccet_ca *ca;
	u32 now;
	s32 delta;

	if (event != CA_EVENT_TX_START)
		return;

	ca = inet_csk_ca(sk);
	now = tcp_jiffies32;
	delta = now - tcp_sk(sk)->lsndtime;

	/* Keep the CUBIC epoch aligned after an application-limited idle. */
	if (ca->epoch_start && delta > 0) {
		ca->epoch_start += delta;
		if ((s32)(ca->epoch_start - now) > 0)
			ca->epoch_start = now;
	}
}

/* Calculate the cubic root using a table and one Newton-Raphson step. */
static u32 roccet_cubic_root(u64 a)
{
	u32 x, b, shift;
	static const u8 v[] = {
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
		return ((u32)v[(u32)a] + 35) >> 6;

	b = ((b * 84) >> 8) - 1;
	shift = a >> (b * 3);
	x = ((u32)(((u32)v[shift] + 10) << b)) >> 6;
	x = 2 * x + (u32)div64_u64(a, (u64)x * (u64)(x - 1));
	x = (x * 341) >> 10;

	return x;
}

static void roccet_cubic_update(struct roccet_ca *ca, u32 cwnd, u32 acked)
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

	if (!ca->epoch_start) {
		ca->epoch_start = tcp_jiffies32;
		ca->ack_cnt = acked;
		ca->tcp_cwnd = cwnd;

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
		} else {
			ca->bic_K =
				roccet_cubic_root(cube_factor *
					(ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
		}
	}

	t = (s32)(tcp_jiffies32 - ca->epoch_start);
	t += usecs_to_jiffies(ca->delay_min);
	t <<= ROCCET_HZ;
	do_div(t, HZ);

	if (t < ca->bic_K)
		offs = ca->bic_K - t;
	else
		offs = t - ca->bic_K;

	delta = (cube_rtt_scale * offs * offs * offs) >>
		(10 + 3 * ROCCET_HZ);
	if (t < ca->bic_K)
		bic_target = ca->bic_origin_point - delta;
	else
		bic_target = ca->bic_origin_point + delta;

	if (bic_target > cwnd)
		ca->cnt = cwnd / (bic_target - cwnd);
	else
		ca->cnt = min_t(u64, (u64)cwnd * 100ULL, U32_MAX);

	if (!ca->last_max_cwnd && ca->cnt > 20)
		ca->cnt = 20;

tcp_friendliness:
	if (tcp_friendliness) {
		u32 scale = beta_scale;

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

	ca->cnt = max(ca->cnt, 2U);
}

static void roccet_update_ack_rate(struct roccet_ca *ca, u32 now, u32 acked)
{
	u32 elapsed;

	if (!ca->ack_rate.interval_start_us) {
		ca->ack_rate.interval_start_us = now ? now : 1U;
		ca->ack_rate.acked = acked;
		return;
	}

	elapsed = now - ca->ack_rate.interval_start_us;
	if (elapsed < ROCCET_ACK_RATE_INTERVAL_US) {
		ca->ack_rate.acked =
			roccet_saturating_add(ca->ack_rate.acked, acked);
		return;
	}

	if (elapsed > ROCCET_ACK_RATE_IDLE_US) {
		ca->ack_rate.last_rate = 0;
		ca->ack_rate.curr_rate = 0;
		ca->ack_rate_samples = 0;
	} else {
		ca->ack_rate.last_rate = ca->ack_rate.curr_rate;
		ca->ack_rate.curr_rate = ca->ack_rate.acked;
		if (ca->ack_rate_samples < 2)
			ca->ack_rate_samples++;
	}

	ca->ack_rate.interval_start_us = now ? now : 1U;
	ca->ack_rate.acked = acked;
}

static bool roccet_ack_rate_plateau(const struct roccet_ca *ca)
{
	if (ca->ack_rate_samples < 2)
		return false;

	if (ca->ack_rate.curr_rate <= ca->ack_rate.last_rate)
		return true;

	return ca->ack_rate.curr_rate - ca->ack_rate.last_rate <=
		ROCCET_ACK_RATE_THRESHOLD;
}

static bool roccet_update_srrtt(struct roccet_ca *ca)
{
	u64 relative_rtt, smoothed;
	u32 delta;

	if (!ca->curr_rtt || !ca->min_rtt)
		return false;

	if (ca->curr_rtt < ca->min_rtt)
		ca->min_rtt = ca->curr_rtt;

	if (!ca->min_rtt)
		return false;

	delta = ca->curr_rtt - ca->min_rtt;
	relative_rtt = div64_u64((u64)delta * 100ULL, ca->min_rtt);
	relative_rtt = min_t(u64, relative_rtt, U32_MAX);

	smoothed =
		(u64)(100U - ROCCET_ALPHA_PERCENT) * ca->curr_srrtt +
		(u64)ROCCET_ALPHA_PERCENT * relative_rtt;
	ca->curr_srrtt =
		min_t(u64, div64_u64(smoothed, 100U), U32_MAX);

	return true;
}

static u32 roccet_jitter_threshold(const struct roccet_ca *ca)
{
	u32 jitter;
	u64 jitter_percent;

	if (!ca->last_rtt)
		jitter = 0;
	else if (ca->last_rtt > ca->curr_rtt)
		jitter = ca->last_rtt - ca->curr_rtt;
	else
		jitter = ca->curr_rtt - ca->last_rtt;

	if (!ca->min_rtt)
		return ROCCET_SRTT_THRESHOLD;

	jitter_percent =
		div64_u64((u64)jitter * 100ULL, ca->min_rtt);

	return min_t(u64, jitter_percent + ROCCET_SRTT_THRESHOLD,
		     U32_MAX);
}

static u32 roccet_beta_cwnd(u32 cwnd)
{
	u64 reduced = div64_u64((u64)cwnd * ROCCET_BETA,
				ROCCET_BETA_SCALE);

	return max_t(u32, min_t(u64, reduced, U32_MAX), 2U);
}

static void roccet_congestion_event(struct sock *sk, u32 now)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct roccet_ca *ca = inet_csk_ca(sk);

	ca->epoch_start = 0;
	ca->last_event_us = now ? now : 1U;
	ca->next_srrtt_check_us = 0;
	ca->cnt = min_t(u64, (u64)tcp_snd_cwnd(tp) * 100ULL, U32_MAX);

	if (tcp_snd_cwnd(tp) > ca->last_max_cwnd)
		ca->last_max_cwnd = tcp_snd_cwnd(tp);

	tcp_snd_cwnd_set(tp, min(tp->snd_cwnd_clamp,
				 roccet_beta_cwnd(tcp_snd_cwnd(tp))));
	tp->snd_ssthresh = tcp_snd_cwnd(tp);
}

static void roccettcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct roccet_ca *ca = inet_csk_ca(sk);
	u32 now = roccet_clock_us(sk);
	bool valid_rtt;

	roccet_update_ack_rate(ca, now, acked);
	valid_rtt = roccet_update_srrtt(ca);

	if (valid_rtt) {
		u32 interval = roccet_eval_interval_us(ca->curr_rtt);

		/* Freeze growth briefly while the standing queue drains. */
		if (ca->last_event_us) {
			if (now - ca->last_event_us <=
			    ROCCET_DRAIN_INTERVAL_US)
				return;
			ca->last_event_us = 0;
		}

		/* LAUNCH exits slow start on RTT growth and an ACK-rate
		 * plateau, or when the application no longer fills the cwnd.
		 */
		if (tcp_in_slow_start(tp) &&
		    ((ca->curr_srrtt > ROCCET_SRTT_THRESHOLD &&
		      roccet_ack_rate_plateau(ca)) ||
		     !tcp_is_cwnd_limited(sk))) {
			u32 cwnd = tcp_snd_cwnd(tp);
			u32 reduced;

			ca->epoch_start = 0;
			if (tp->snd_ssthresh == TCP_INFINITE_SSTHRESH)
				reduced = max(cwnd / 2, TCP_INIT_CWND);
			else
				reduced = cwnd - cwnd / 3;

			tcp_snd_cwnd_set(tp, max(reduced, 2U));
			tp->snd_ssthresh = tcp_snd_cwnd(tp);
			ca->last_event_us = now ? now : 1U;
			return;
		}

		/* ORBITER evaluates relative RTT every five RTTs. */
		if (!tcp_in_slow_start(tp) && interval) {
			bool evaluate = false;

			if (!ca->next_srrtt_check_us) {
				ca->next_srrtt_check_us = now + interval;
			} else if (roccet_time_after_eq(now,
					   ca->next_srrtt_check_us)) {
				evaluate = true;
				ca->next_srrtt_check_us = now + interval;
			}

			if (evaluate &&
			    ca->curr_srrtt > roccet_jitter_threshold(ca)) {
				roccet_congestion_event(sk, now);
				return;
			}
		}
	}

	/* Without a valid RTT sample, or without a ROCCET event, retain
	 * the current in-tree CUBIC window-control behavior.
	 */
	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tcp_in_slow_start(tp)) {
		acked = tcp_slow_start(tp, acked);
		if (!acked)
			return;
	}

	roccet_cubic_update(ca, tcp_snd_cwnd(tp), acked);
	tcp_cong_avoid_ai(tp, ca->cnt, acked);
}

static u32 roccettcp_recalc_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct roccet_ca *ca = inet_csk_ca(sk);
	u32 cwnd = tcp_snd_cwnd(tp);

	/* LAUNCH does not treat an early cellular loss as its exit signal. */
	if (tcp_in_slow_start(tp))
		return cwnd;

	ca->epoch_start = 0;

	if (cwnd < ca->last_max_cwnd && fast_convergence) {
		u64 converged =
			(u64)cwnd * (ROCCET_BETA_SCALE + ROCCET_BETA);

		ca->last_max_cwnd =
			min_t(u64, div64_u64(converged,
					     2U * ROCCET_BETA_SCALE),
			      U32_MAX);
	} else {
		ca->last_max_cwnd = cwnd;
	}

	return roccet_beta_cwnd(cwnd);
}

static void roccettcp_state(struct sock *sk, u8 new_state)
{
	if (new_state == TCP_CA_Loss)
		roccettcp_reset(inet_csk_ca(sk));
}

static void roccettcp_acked(struct sock *sk,
			    const struct ack_sample *sample)
{
	struct roccet_ca *ca = inet_csk_ca(sk);
	u32 delay;

	if (sample->rtt_us <= 0)
		return;

	/* Match CUBIC's filtering of samples immediately after recovery. */
	if (ca->epoch_start &&
	    (s32)(tcp_jiffies32 - ca->epoch_start) < HZ)
		return;

	delay = sample->rtt_us;
	if (!ca->delay_min || delay < ca->delay_min)
		ca->delay_min = delay;

	if (!ca->min_rtt || delay < ca->min_rtt)
		ca->min_rtt = delay;

	if (!ca->curr_rtt) {
		ca->last_rtt = 0;
		ca->curr_rtt = delay;
	} else {
		ca->last_rtt = ca->curr_rtt;
		ca->curr_rtt = delay;
	}
}

static struct tcp_congestion_ops roccet __read_mostly = {
	.init		= roccettcp_init,
	.ssthresh	= roccettcp_recalc_ssthresh,
	.cong_avoid	= roccettcp_cong_avoid,
	.set_state	= roccettcp_state,
	.undo_cwnd	= tcp_reno_undo_cwnd,
	.cwnd_event	= roccettcp_cwnd_event,
	.pkts_acked	= roccettcp_acked,
	.owner		= THIS_MODULE,
	.name		= "roccet",
};

static int __init roccettcp_register(void)
{
	BUILD_BUG_ON(sizeof(struct roccet_ca) > ICSK_CA_PRIV_SIZE);
	BUILD_BUG_ON(!ROCCET_BETA ||
		     ROCCET_BETA >= ROCCET_BETA_SCALE);
	BUILD_BUG_ON(!ROCCET_BIC_SCALE);
	BUILD_BUG_ON(!ROCCET_ALPHA_PERCENT ||
		     ROCCET_ALPHA_PERCENT >= 100U);
	BUILD_BUG_ON(!ROCCET_SRTT_THRESHOLD);
	BUILD_BUG_ON(!ROCCET_ACK_RATE_THRESHOLD);
	BUILD_BUG_ON(!ROCCET_ACK_RATE_INTERVAL_US);
	BUILD_BUG_ON(!ROCCET_DRAIN_INTERVAL_US);
	BUILD_BUG_ON(!ROCCET_EVAL_RTT_COUNT);
	BUILD_BUG_ON(10U + 3U * ROCCET_HZ >= 64U);

	beta_scale =
		8U * (ROCCET_BETA_SCALE + ROCCET_BETA) / 3U /
		(ROCCET_BETA_SCALE - ROCCET_BETA);
	cube_rtt_scale = ROCCET_BIC_SCALE * 10U;

	cube_factor = 1ULL << (10U + 3U * ROCCET_HZ);
	do_div(cube_factor, ROCCET_BIC_SCALE * 10U);

	return tcp_register_congestion_control(&roccet);
}

static void __exit roccettcp_unregister(void)
{
	tcp_unregister_congestion_control(&roccet);
}

module_init(roccettcp_register);
module_exit(roccettcp_unregister);

MODULE_AUTHOR("Lukas Prause");
MODULE_AUTHOR("Mark Akselrod");
MODULE_AUTHOR("Tim Fuechsel");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ROCCET TCP congestion control");
