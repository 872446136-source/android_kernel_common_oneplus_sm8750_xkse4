// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <linux/vmalloc.h>
#include <linux/lz4kd.h>
#include <crypto/algapi.h>

struct lz4kd_ctx {
	void *workmem;
};

static int lz4kd_init(struct crypto_tfm *tfm)
{
	struct lz4kd_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->workmem = vmalloc(lz4kd_encode_state_bytes_min());
	return ctx->workmem ? 0 : -ENOMEM;
}

static void lz4kd_exit(struct crypto_tfm *tfm)
{
	struct lz4kd_ctx *ctx = crypto_tfm_ctx(tfm);

	vfree(ctx->workmem);
}

static int lz4kd_compress_crypto(struct crypto_tfm *tfm, const u8 *src,
		unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct lz4kd_ctx *ctx = crypto_tfm_ctx(tfm);
	int ret = lz4kd_encode(ctx->workmem, src, dst, slen, *dlen, 0);

	if (ret <= 0)
		return -EINVAL;
	*dlen = ret;
	return 0;
}

static int lz4kd_decompress_crypto(struct crypto_tfm *tfm, const u8 *src,
		unsigned int slen, u8 *dst, unsigned int *dlen)
{
	int ret = lz4kd_decode(src, dst, slen, *dlen);

	if (ret <= 0)
		return -EINVAL;
	*dlen = ret;
	return 0;
}

static struct crypto_alg alg_lz4kd = {
	.cra_name = "lz4kd",
	.cra_driver_name = "lz4kd-generic",
	.cra_flags = CRYPTO_ALG_TYPE_COMPRESS,
	.cra_ctxsize = sizeof(struct lz4kd_ctx),
	.cra_module = THIS_MODULE,
	.cra_init = lz4kd_init,
	.cra_exit = lz4kd_exit,
	.cra_u = { .compress = {
		.coa_compress = lz4kd_compress_crypto,
		.coa_decompress = lz4kd_decompress_crypto,
	} },
};

static int __init lz4kd_mod_init(void)
{
	return crypto_register_alg(&alg_lz4kd);
}

static void __exit lz4kd_mod_exit(void)
{
	crypto_unregister_alg(&alg_lz4kd);
}

subsys_initcall(lz4kd_mod_init);
module_exit(lz4kd_mod_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Huawei LZ4KD compression algorithm");
MODULE_ALIAS_CRYPTO("lz4kd");
