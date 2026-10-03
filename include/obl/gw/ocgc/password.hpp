#pragma once

// OCG-C Logon password encryption (§3.5, §3.6):
//   plaintext  = login time in UTC "YYYYMMDDHHMMSS" + password
//   ciphertext = RSA-2048 with HKEX's public key, PKCS #1 v1.5 or OAEP padding, big endian
//   field      = base-64 of the ciphertext (344 characters for a 2048-bit key)
// OCG-C decrypts, checks the login time against its clock within a tolerance, then the password.
//
// Requires OpenSSL (libcrypto); compiled only when OBL_HAVE_OPENSSL is defined.

#if defined(OBL_HAVE_OPENSSL)

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace obl::gw::ocgc {

enum class RsaPadding { Pkcs1, Oaep };

namespace detail {
struct PkeyFree {
  void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct CtxFree {
  void operator()(EVP_PKEY_CTX* p) const { EVP_PKEY_CTX_free(p); }
};
using Pkey = std::unique_ptr<EVP_PKEY, PkeyFree>;
using Ctx = std::unique_ptr<EVP_PKEY_CTX, CtxFree>;

inline Pkey read_key(std::string_view pem, bool is_private) {
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (!bio) return nullptr;
  EVP_PKEY* k = is_private ? PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr)
                           : PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return Pkey(k);
}

inline bool set_padding(EVP_PKEY_CTX* c, RsaPadding p) {
  return EVP_PKEY_CTX_set_rsa_padding(c, p == RsaPadding::Oaep ? RSA_PKCS1_OAEP_PADDING : RSA_PKCS1_PADDING) > 0;
}

inline std::string base64(const unsigned char* p, std::size_t n) {
  std::string out(4 * ((n + 2) / 3), '\0');
  const int len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), p, static_cast<int>(n));
  out.resize(static_cast<std::size_t>(len));
  return out;
}

inline std::optional<std::string> unbase64(std::string_view s) {
  std::string out(3 * s.size() / 4 + 3, '\0');
  const int len = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(s.data()), static_cast<int>(s.size()));
  if (len < 0) return std::nullopt;
  std::size_t n = static_cast<std::size_t>(len);
  // EVP_DecodeBlock counts '=' padding as zero bytes
  for (std::size_t i = s.size(); i > 0 && s[i - 1] == '='; --i) --n;
  out.resize(n);
  return out;
}
}  // namespace detail

// "YYYYMMDDHHMMSS" for a UTC time in seconds since the epoch.
inline std::string utc_stamp(std::time_t t) {
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y%m%d%H%M%S", &tm);
  return buf;
}

// Returns the base-64 field value, or nullopt if the key cannot be used.
inline std::optional<std::string> encrypt_password(std::string_view public_key_pem, std::string_view password,
                                                   std::time_t login_utc, RsaPadding pad = RsaPadding::Pkcs1) {
  detail::Pkey key = detail::read_key(public_key_pem, false);
  if (!key) return std::nullopt;
  detail::Ctx ctx(EVP_PKEY_CTX_new(key.get(), nullptr));
  if (!ctx || EVP_PKEY_encrypt_init(ctx.get()) <= 0 || !detail::set_padding(ctx.get(), pad)) return std::nullopt;
  const std::string plain = utc_stamp(login_utc) + std::string(password);
  std::size_t out_len = 0;
  const auto* in = reinterpret_cast<const unsigned char*>(plain.data());
  if (EVP_PKEY_encrypt(ctx.get(), nullptr, &out_len, in, plain.size()) <= 0) return std::nullopt;
  std::string cipher(out_len, '\0');
  if (EVP_PKEY_encrypt(ctx.get(), reinterpret_cast<unsigned char*>(cipher.data()), &out_len, in, plain.size()) <= 0)
    return std::nullopt;
  return detail::base64(reinterpret_cast<const unsigned char*>(cipher.data()), out_len);
}

// The exchange side (simulator): decrypt and split into login time and password.
struct DecryptedPassword {
  std::time_t login_utc = 0;
  std::string password;
};

inline std::optional<DecryptedPassword> decrypt_password(std::string_view private_key_pem, std::string_view field,
                                                         RsaPadding pad = RsaPadding::Pkcs1) {
  detail::Pkey key = detail::read_key(private_key_pem, true);
  auto cipher = detail::unbase64(field);
  if (!key || !cipher) return std::nullopt;
  detail::Ctx ctx(EVP_PKEY_CTX_new(key.get(), nullptr));
  if (!ctx || EVP_PKEY_decrypt_init(ctx.get()) <= 0 || !detail::set_padding(ctx.get(), pad)) return std::nullopt;
  std::size_t out_len = 0;
  const auto* in = reinterpret_cast<const unsigned char*>(cipher->data());
  if (EVP_PKEY_decrypt(ctx.get(), nullptr, &out_len, in, cipher->size()) <= 0) return std::nullopt;
  std::string plain(out_len, '\0');
  if (EVP_PKEY_decrypt(ctx.get(), reinterpret_cast<unsigned char*>(plain.data()), &out_len, in, cipher->size()) <= 0)
    return std::nullopt;
  plain.resize(out_len);
  if (plain.size() < 14) return std::nullopt;
  std::tm tm{};
  if (!strptime(plain.substr(0, 14).c_str(), "%Y%m%d%H%M%S", &tm)) return std::nullopt;
  return DecryptedPassword{timegm(&tm), plain.substr(14)};
}

}  // namespace obl::gw::ocgc

#endif
