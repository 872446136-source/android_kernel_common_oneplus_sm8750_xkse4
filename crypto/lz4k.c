// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <linux/slab.h>
#include <linux/lz4k.h>
#include <crypto/algapi.h>

struct lz4k_ctx {
	void *workmem;
};

static int lz4k_init(struct crypto_tfm *tfm)
{
	struct lz4k_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->workmem = kvzalloc(lz4k_state_bytes_min(), GFP_KERNEL | __GFP_NOWARN);
	return ctx->workmem ? 0 : -ENOMEM;
}

static void lz4k_exit(struct crypto_tfm *tfm)
{
	struct lz4k_ctx *ctx = crypto_tfm_ctx(tfm);

	kvfree(ctx->workmem);
}

static int lz4k_compress_crypto(struct crypto_tfm *tfm, const u8 *src,
		unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct lz4k_ctx *ctx = crypto_tfm_ctx(tfm);
	int ret = lz4k_compress(ctx->workmem, src, dst, slen, *dlen);

	if (ret < 0)
		return -ENOSPC;
	*dlen = ret;
	return 0;
}

static int lz4k_decompress_crypto(struct crypto_tfm *tfm, const u8 *src,
		unsigned int slen, u8 *dst, unsigned int *dlen)
{
	int ret = lz4k_decompress(src, dst, slen, *dlen);

	if (ret <= 0)
		return -EINVAL;
	*dlen = ret;
	return 0;
}

static struct crypto_alg alg_lz4k = {
	.cra_name = "lz4k",
	.cra_driver_name = "lz4k-generic",
	.cra_flags = CRYPTO_ALG_TYPE_COMPRESS,
	.cra_ctxsize = sizeof(struct lz4k_ctx),
	.cra_module = THIS_MODULE,
	.cra_init = lz4k_init,
	.cra_exit = lz4k_exit,
	.cra_u = { .compress = {
		.coa_compress = lz4k_compress_crypto,
		.coa_decompress = lz4k_decompress_crypto,
	} },
};

static int __init lz4k_mod_init(void)
{
	return crypto_register_alg(&alg_lz4k);
}

static void __exit lz4k_mod_exit(void)
{
	crypto_unregister_alg(&alg_lz4k);
}

subsys_initcall(lz4k_mod_init);
module_exit(lz4k_mod_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Oplus LZ4K compression algorithm");
MODULE_ALIAS_CRYPTO("lz4k");
