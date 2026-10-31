#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif
namespace cafe::host {
template<size_t N> inline std::array<uint8_t,N> digest(std::span<const uint8_t> data) {
    static_assert(N == 16 || N == 32);
    std::array<uint8_t,N> out{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    auto status=BCryptOpenAlgorithmProvider(&algorithm,N==16?BCRYPT_MD5_ALGORITHM:BCRYPT_SHA256_ALGORITHM,nullptr,0);
    if(status>=0) status=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);
    while(status>=0 && !data.empty()) {
        const auto n=static_cast<ULONG>(std::min<size_t>(data.size(),0xffffffffu));
        status=BCryptHashData(hash,const_cast<PUCHAR>(data.data()),n,0);
        data=data.subspan(n);
    }
    if(status>=0) status=BCryptFinishHash(hash,out.data(),out.size(),0);
    if(hash) BCryptDestroyHash(hash);
    if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    if(status<0) throw std::runtime_error("Windows digest failed");
#else
    unsigned length=0;
    if(EVP_Digest(data.data(),data.size(),out.data(),&length,N==16?EVP_md5():EVP_sha256(),nullptr)!=1 || length!=N)
        throw std::runtime_error("digest failed");
#endif
    return out;
}
} // namespace cafe::host
