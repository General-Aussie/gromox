#pragma once
#include <cstdint>
#include <memory>
#include <openssl/evp.h>

namespace gromox {

struct sslfree {
	void operator()(EVP_CIPHER_CTX *x) { EVP_CIPHER_CTX_free(x); }
	void operator()(EVP_MD_CTX *x) { EVP_MD_CTX_free(x); }
};

struct HMACMD5_CTX {
	HMACMD5_CTX() = default;
	HMACMD5_CTX(const void *key, size_t len);
	bool update(const void *text, size_t len);
	bool finish(void *output);
	bool is_valid() const { return valid_flag; }

	protected:
	std::unique_ptr<EVP_MD_CTX, sslfree> osslctx;
	uint8_t k_ipad[65]{}, k_opad[65]{};
	bool valid_flag = false;
};

}
