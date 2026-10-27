// -*- coding: utf-8 -*-
// Pipeline F phase-10: TrustAnchorManager — see header.
#include "TrustAnchorManager.h"

#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/sha.h>
#include <openssl/err.h>

#include <QSettings>

#include <fstream>

namespace pdfe::signatures {

namespace {

constexpr char kSettingsGroup[] = "TrustAnchors";

// Compute uppercase-hex SHA-256 of a byte buffer. The OpenSSL X509 API
// has X509_digest, but we need it for the raw DER (which is the unique
// identity in QSettings) so we hash directly. 64-char hex, no separators.
QString sha256HexUpper(const unsigned char* data, std::size_t len)
{
    if (!data || len == 0) return {};
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(data, len, digest);

    static const char* k = "0123456789ABCDEF";
    QString out;
    out.reserve(SHA256_DIGEST_LENGTH * 2);
    for (unsigned int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        out.append(QChar(k[(digest[i] >> 4) & 0xF]));
        out.append(QChar(k[digest[i] & 0xF]));
    }
    return out;
}

// Pull a Common Name out of an X509_NAME. Returns empty string if no CN
// attribute is present. UTF-8 output.
QString extractCommonName(X509_NAME* name)
{
    if (!name) return {};
    int idx = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
    if (idx < 0) return {};
    X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, idx);
    if (!entry) return {};
    ASN1_STRING* asn1 = X509_NAME_ENTRY_get_data(entry);
    if (!asn1) return {};
    unsigned char* utf8 = nullptr;
    int len = ASN1_STRING_to_UTF8(&utf8, asn1);
    if (len <= 0 || !utf8) {
        if (utf8) OPENSSL_free(utf8);
        return {};
    }
    QString out = QString::fromUtf8(reinterpret_cast<const char*>(utf8), len);
    OPENSSL_free(utf8);
    return out;
}

// Read a whole file into a byte vector. Bounded at 1 MiB — a single
// X.509 cert is typically <2 KiB; anything bigger is suspicious.
constexpr std::size_t kMaxCertBytes = 1u * 1024u * 1024u;

bool readWholeFile(const std::filesystem::path& path, QByteArray& out,
                   QString* errorOut)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (errorOut) *errorOut = QStringLiteral("File not found");
        return false;
    }
    auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz == 0) {
        if (errorOut) *errorOut = QStringLiteral("Empty or unreadable file");
        return false;
    }
    if (sz > kMaxCertBytes) {
        if (errorOut) *errorOut = QStringLiteral("File too large for X.509 cert");
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (errorOut) *errorOut = QStringLiteral("Cannot open file");
        return false;
    }
    out.resize(static_cast<int>(sz));
    f.read(out.data(), static_cast<std::streamsize>(sz));
    if (!f) {
        if (errorOut) *errorOut = QStringLiteral("Short read");
        out.clear();
        return false;
    }
    return true;
}

} // namespace

TrustAnchorManager& TrustAnchorManager::instance()
{
    static TrustAnchorManager s_instance;
    return s_instance;
}

TrustAnchorManager::TrustAnchorManager()
{
    loadFromSettings();
}

void TrustAnchorManager::loadFromSettings()
{
    anchors_.clear();
    QSettings s; // Uses QCoreApplication org/app names — same as AppSettings.
    s.beginGroup(kSettingsGroup);
    const QStringList fingerprints = s.childGroups();
    for (const QString& fp : fingerprints) {
        s.beginGroup(fp);
        TrustAnchor a;
        a.fingerprintSha256 = fp;
        a.filePath  = s.value(QStringLiteral("path")).toString();
        a.subjectCN = s.value(QStringLiteral("subject")).toString();
        a.issuerCN  = s.value(QStringLiteral("issuer")).toString();
        const QString der_b64 = s.value(QStringLiteral("der_b64")).toString();
        a.derBytes = QByteArray::fromBase64(der_b64.toLatin1());
        s.endGroup();
        if (!a.derBytes.isEmpty() && !a.fingerprintSha256.isEmpty()) {
            anchors_.append(std::move(a));
        }
    }
    s.endGroup();
}

void TrustAnchorManager::writeAnchorToSettings(const TrustAnchor& a)
{
    QSettings s;
    s.beginGroup(kSettingsGroup);
    s.beginGroup(a.fingerprintSha256);
    s.setValue(QStringLiteral("path"),    a.filePath);
    s.setValue(QStringLiteral("subject"), a.subjectCN);
    s.setValue(QStringLiteral("issuer"),  a.issuerCN);
    s.setValue(QStringLiteral("der_b64"), QString::fromLatin1(a.derBytes.toBase64()));
    s.endGroup();
    s.endGroup();
    s.sync();
}

void TrustAnchorManager::removeAnchorFromSettings(const QString& fp)
{
    QSettings s;
    s.beginGroup(kSettingsGroup);
    s.remove(fp);  // Removes the whole sub-group.
    s.endGroup();
    s.sync();
}

bool TrustAnchorManager::loadCertBytes(const std::filesystem::path& path,
                                        QByteArray& derOut,
                                        QString* errorOut)
{
    QByteArray raw;
    if (!readWholeFile(path, raw, errorOut)) return false;

    // Try PEM first. PEM_read_bio_X509 sniffs the "-----BEGIN" header
    // so it'll quietly fail on a binary DER without complaining.
    BIO* bio = BIO_new_mem_buf(raw.constData(), static_cast<int>(raw.size()));
    if (!bio) {
        if (errorOut) *errorOut = QStringLiteral("BIO allocation failed");
        return false;
    }
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (cert) {
        // Re-encode to canonical DER. PEM and the original DER may differ
        // bit-for-bit (whitespace etc); the fingerprint must hash the
        // canonical DER so QSettings dedup works.
        unsigned char* der = nullptr;
        int derLen = i2d_X509(cert, &der);
        X509_free(cert);
        if (derLen <= 0 || !der) {
            if (der) OPENSSL_free(der);
            if (errorOut) *errorOut = QStringLiteral("i2d_X509 failed");
            return false;
        }
        derOut = QByteArray(reinterpret_cast<const char*>(der), derLen);
        OPENSSL_free(der);
        return true;
    }
    // Drain any PEM-parsing errors before falling back, otherwise OpenSSL
    // will report them on the next op as if we'd ignored them.
    ERR_clear_error();

    // Fall back to raw DER.
    const unsigned char* p = reinterpret_cast<const unsigned char*>(raw.constData());
    cert = d2i_X509(nullptr, &p, static_cast<long>(raw.size()));
    if (!cert) {
        ERR_clear_error();
        if (errorOut) *errorOut = QStringLiteral("Not a valid PEM or DER X.509 certificate");
        return false;
    }
    unsigned char* der = nullptr;
    int derLen = i2d_X509(cert, &der);
    X509_free(cert);
    if (derLen <= 0 || !der) {
        if (der) OPENSSL_free(der);
        if (errorOut) *errorOut = QStringLiteral("i2d_X509 failed");
        return false;
    }
    derOut = QByteArray(reinterpret_cast<const char*>(der), derLen);
    OPENSSL_free(der);
    return true;
}

bool TrustAnchorManager::buildAnchorFromDer(const QByteArray& der,
                                              TrustAnchor& a)
{
    if (der.isEmpty()) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.constData());
    X509* cert = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
    if (!cert) {
        ERR_clear_error();
        return false;
    }
    a.derBytes = der;
    a.fingerprintSha256 = sha256HexUpper(
        reinterpret_cast<const unsigned char*>(der.constData()), der.size());
    a.subjectCN = extractCommonName(X509_get_subject_name(cert));
    a.issuerCN  = extractCommonName(X509_get_issuer_name(cert));
    X509_free(cert);
    return !a.fingerprintSha256.isEmpty();
}

bool TrustAnchorManager::addTrust(const std::filesystem::path& certPath,
                                    QString* errorOut)
{
    QByteArray der;
    if (!loadCertBytes(certPath, der, errorOut)) {
        return false;
    }

    TrustAnchor a;
    if (!buildAnchorFromDer(der, a)) {
        if (errorOut) *errorOut = QStringLiteral("Cert parses but no fingerprint");
        return false;
    }
    a.filePath = QString::fromStdString(certPath.string());

    // Idempotent — if the fingerprint already exists, return success but
    // don't double-store.
    for (const auto& existing : anchors_) {
        if (existing.fingerprintSha256 == a.fingerprintSha256) {
            if (errorOut) *errorOut = QStringLiteral("Already trusted");
            return true;
        }
    }

    anchors_.append(a);
    writeAnchorToSettings(a);
    return true;
}

bool TrustAnchorManager::removeTrust(const QString& fingerprint)
{
    for (int i = 0; i < anchors_.size(); ++i) {
        if (anchors_[i].fingerprintSha256 == fingerprint) {
            anchors_.removeAt(i);
            removeAnchorFromSettings(fingerprint);
            return true;
        }
    }
    return false;
}

QList<TrustAnchor> TrustAnchorManager::listTrusts() const
{
    return anchors_;
}

TrustAnchorSet TrustAnchorManager::snapshot() const
{
    TrustAnchorSet set;
    for (const auto& a : anchors_) {
        // addDer rejects malformed DER, which is the same tolerance the
        // previous inline loop had — a bad entry is skipped, it does not
        // fail the whole snapshot.
        set.addDer(a.derBytes);
    }
    return set;
}

void TrustAnchorManager::loadIntoStore(X509_STORE* store) const
{
    if (!store) return;
    snapshot().loadIntoStore(store);
}

void TrustAnchorManager::clear()
{
    QSettings s;
    s.beginGroup(kSettingsGroup);
    s.remove(QString());  // Wipe entire group.
    s.endGroup();
    s.sync();
    anchors_.clear();
}

} // namespace pdfe::signatures
