/* SPDX-License-Identifier: BSD-3-Clause */

#include <string.h>

#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/core_object.h>
#include <openssl/params.h>

#include "tpm2-provider-pkey.h"

typedef struct tpm2_object_ctx_st TPM2_OBJECT_CTX;

struct tpm2_object_ctx_st {
    const OSSL_CORE_HANDLE *core;
    ESYS_CONTEXT *esys_ctx;
    TPMS_CAPABILITY_DATA *capability;
    int has_pass;
    TPM2_HANDLE handle;
    BIO *bio;
    int load_done;
};

static OSSL_FUNC_store_open_fn tpm2_object_open;
static OSSL_FUNC_store_attach_fn tpm2_object_attach;
static OSSL_FUNC_store_settable_ctx_params_fn tpm2_object_settable_params;
static OSSL_FUNC_store_set_ctx_params_fn tpm2_object_set_params;
static OSSL_FUNC_store_load_fn tpm2_object_load;
static OSSL_FUNC_store_eof_fn tpm2_object_eof;
static OSSL_FUNC_store_close_fn tpm2_object_close;

static void *
tpm2_object_open(void *provctx, const char *uri)
{
    TPM2_PROVIDER_CTX *cprov = provctx;
    TPM2_OBJECT_CTX *ctx;
    char *baseuri, *opts;

    DBG("STORE/OBJECT OPEN %s\n", uri);
    if ((ctx = OPENSSL_zalloc(sizeof(TPM2_OBJECT_CTX))) == NULL)
        return NULL;

    ctx->core = cprov->core;
    ctx->esys_ctx = cprov->esys_ctx;
    ctx->capability = cprov->capability;

    if ((baseuri = OPENSSL_strdup(uri)) == NULL)
        goto error1;
    if ((opts = strchr(baseuri, '?')) != NULL) {
        *opts = 0;

        if (!strncmp(opts+1, "pass", 4))
            ctx->has_pass = 1;
        else
            goto error2;
    }

    /* the object is stored in a file */
    if (!strncmp(baseuri, "object:", 7)) {
        if ((ctx->bio = BIO_new_file(baseuri+7, "rb")) == NULL)
            goto error2;
    /* the object is persisted under a specific handle */
    } else if (!strncmp(baseuri, "handle:", 7)) {
        unsigned long int value;
        char *end_ptr = NULL;

        value = strtoul(baseuri+7, &end_ptr, 16);
        if (*end_ptr != 0 || value > UINT32_MAX)
            goto error2;

        ctx->handle = value;
    } else
        goto error2;

    OPENSSL_free(baseuri);
    return ctx;
error2:
    OPENSSL_free(baseuri);
error1:
    OPENSSL_clear_free(ctx, sizeof(TPM2_OBJECT_CTX));
    return NULL;
}

static void *
tpm2_object_attach(void *provctx, OSSL_CORE_BIO *cin)
{
    TPM2_PROVIDER_CTX *cprov = provctx;
    TPM2_OBJECT_CTX *ctx;

    DBG("STORE/OBJECT ATTACH\n");
    if ((ctx = OPENSSL_zalloc(sizeof(TPM2_OBJECT_CTX))) == NULL)
        return NULL;

    ctx->core = cprov->core;
    ctx->esys_ctx = cprov->esys_ctx;
    ctx->capability = cprov->capability;

    if ((ctx->bio = bio_new_from_core_bio(cprov->corebiometh, cin)) == NULL)
        goto error;

    return ctx;
error:
    OPENSSL_clear_free(ctx, sizeof(TPM2_OBJECT_CTX));
    return NULL;
}

static const OSSL_PARAM *
tpm2_object_settable_params(void *provctx)
{
    static const OSSL_PARAM known_settable_ctx_params[] = {
        OSSL_PARAM_END
    };
    return known_settable_ctx_params;
}

static int
tpm2_object_set_params(void *loaderctx, const OSSL_PARAM params[])
{
    TRACE_PARAMS("STORE/OBJECT SET_PARAMS", params);
    return 1;
}

static int
read_until_eof(BIO *bio, uint8_t **buffer)
{
    int size = 1024;
    int len = 0;

    if ((*buffer = OPENSSL_malloc(size)) == NULL)
        return -1;
    /* read until the end-of-file */
    do {
        int res;

        if (size - len < 64) {
            uint8_t *newbuff;

            size += 1024;
            if ((newbuff = OPENSSL_realloc(*buffer, size)) == NULL)
                goto error;

            *buffer = newbuff;
        }

        res = BIO_read(bio, *buffer + len, size - len);
        if (res < 0)
            goto error;
        len += res;
    } while (!BIO_eof(bio));

    return len;
error:
    OPENSSL_free(*buffer);
    return -1;
}

static int
tpm2_object_load(void *ctx,
            OSSL_CALLBACK *object_cb, void *object_cbarg,
            OSSL_PASSPHRASE_CALLBACK *pw_cb, void *pw_cbarg)
{
    TPM2_OBJECT_CTX *sctx = ctx;
    TPM2B_PUBLIC *out_public = NULL;
    TPM2_PKEY *pkey = NULL;
    TSS2_RC r;

    DBG("STORE/OBJECT LOAD\n");
    pkey = OPENSSL_zalloc(sizeof(TPM2_PKEY));
    if (pkey == NULL)
        return 0;

    pkey->core = sctx->core;
    pkey->esys_ctx = sctx->esys_ctx;
    pkey->capability = sctx->capability;

    if (sctx->bio) {
        uint8_t *buffer;
        int buffer_size;

        if ((buffer_size = read_until_eof(sctx->bio, &buffer)) < 0)
            goto error1;
        /* read object metadata */
        r = Esys_TR_Deserialize(sctx->esys_ctx, buffer, buffer_size, &pkey->object);
        OPENSSL_free(buffer);
    } else {
        /* create reference to a pre-existing TPM object */
        r = Esys_TR_FromTPMPublic(sctx->esys_ctx, sctx->handle,
                                  ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                                  &pkey->object);
        sctx->load_done = 1;
    }
    TPM2_CHECK_RC(sctx->core, r, TPM2_ERR_CANNOT_LOAD_KEY, goto error1);

    if (sctx->has_pass) {
        TPM2B_DIGEST userauth;
        size_t plen = 0;

        /* request password; this might open an interactive user prompt */
        if (!pw_cb((char *)userauth.buffer, sizeof(TPMU_HA), &plen, NULL, pw_cbarg)) {
            TPM2_ERROR_raise(sctx->core, TPM2_ERR_AUTHORIZATION_FAILURE);
            goto error2;
        }
        userauth.size = plen;

        r = Esys_TR_SetAuth(sctx->esys_ctx, pkey->object, &userauth);
        TPM2_CHECK_RC(sctx->core, r, TPM2_ERR_CANNOT_LOAD_KEY, goto error2);
    } else
        pkey->data.emptyAuth = 1;

    r = Esys_ReadPublic(sctx->esys_ctx, pkey->object,
                        ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                        &out_public, NULL, NULL);
    TPM2_CHECK_RC(sctx->core, r, TPM2_ERR_CANNOT_LOAD_KEY, goto error2);

    pkey->data.pub = *out_public;
    pkey->data.privatetype = KEY_TYPE_HANDLE;
    pkey->data.handle = sctx->handle;

    free(out_public);

    OSSL_PARAM params[4];
    int object_type = OSSL_OBJECT_PKEY;
    const char *keytype;

    params[0] = OSSL_PARAM_construct_int(OSSL_OBJECT_PARAM_TYPE, &object_type);

    if ((keytype = tpm2_openssl_type(&pkey->data)) == NULL) {
        TPM2_ERROR_raise(sctx->core, TPM2_ERR_UNKNOWN_ALGORITHM);
        goto error2;
    }
    DBG("STORE/OBJECT LOAD found %s\n", keytype);
    params[1] = OSSL_PARAM_construct_utf8_string(OSSL_OBJECT_PARAM_DATA_TYPE,
                                                 (char *)keytype, 0);
    /* The address of the key becomes the octet string */
    params[2] = OSSL_PARAM_construct_octet_string(OSSL_OBJECT_PARAM_REFERENCE,
                                                  &pkey, sizeof(pkey));
    params[3] = OSSL_PARAM_construct_end();

    return object_cb(params, object_cbarg);
error2:
    Esys_TR_Close(sctx->esys_ctx, &pkey->object);
error1:
    OPENSSL_clear_free(pkey, sizeof(TPM2_PKEY));
    return 0;
}

static int
tpm2_object_eof(void *ctx)
{
    TPM2_OBJECT_CTX *sctx = ctx;
    return (sctx->bio && BIO_eof(sctx->bio)) || sctx->load_done;
}

static int
tpm2_object_close(void *ctx)
{
    TPM2_OBJECT_CTX *sctx = ctx;

    if (sctx == NULL)
        return 0;

    DBG("STORE/OBJECT CLOSE\n");
    BIO_free(sctx->bio);

    OPENSSL_clear_free(ctx, sizeof(TPM2_OBJECT_CTX));
    return 1;
}

const OSSL_DISPATCH tpm2_object_store_functions[] = {
    { OSSL_FUNC_STORE_OPEN, (void(*)(void))tpm2_object_open },
    { OSSL_FUNC_STORE_ATTACH, (void(*)(void))tpm2_object_attach },
    { OSSL_FUNC_STORE_SETTABLE_CTX_PARAMS, (void(*)(void))tpm2_object_settable_params },
    { OSSL_FUNC_STORE_SET_CTX_PARAMS, (void(*)(void))tpm2_object_set_params },
    { OSSL_FUNC_STORE_LOAD, (void(*)(void))tpm2_object_load },
    { OSSL_FUNC_STORE_EOF, (void(*)(void))tpm2_object_eof },
    { OSSL_FUNC_STORE_CLOSE, (void(*)(void))tpm2_object_close },
    { 0, NULL }
};

