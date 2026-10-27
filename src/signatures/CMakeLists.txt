add_library(pdfe_signatures STATIC
    Pkcs7OpenSslBridge.cpp
    CertChainExtractor.cpp
    LtvDssWriter.cpp
    OcspResponseParser.cpp
    CrlParser.cpp
    OcspUrlExtractor.cpp
    CrlUrlExtractor.cpp
    SignatureManager.cpp
    SignatureCommands.cpp
    # Meta-audit follow-up F2 (PDF-31/PDF-32): the READ side of /Perms /DocMDP.
    DocMdpEnforcer.cpp
    # Meta-audit follow-up F24: the CONSUMER for the above — a save-path guard
    # that refuses to overwrite a certified original whose certification the
    # pending changes break, and warns on Save As. Without this, DocMdpEnforcer
    # was detection nothing ever asked.
    DocMdpSaveGuard.cpp
    # F24 REMEDIATION (security-audit gate, finding 1): the same policy
    # for the four engines that call pdf_save_document() directly and so
    # never meet the Document-side save guard.
    DocMdpDirectWriteGuard.cpp
    # Meta-audit follow-up F21 (PDF-36/PDF-37): does /ByteRange actually
    # COVER the file the signature claims to vouch for?
    SignatureCoverageAnalyzer.cpp
    TsaClient.cpp
    ValidationFetcher.cpp
    ValidationCoordinator.cpp
    # Pipeline F phase-10: offline LTV verification + trust anchors.
    DssReader.cpp
    TrustAnchorManager.cpp
    # Meta-audit follow-up F23 (PDF-39): the trust-store seam — an explicit,
    # copyable anchor set (TrustAnchorSet) plus chain validation that takes it
    # as a parameter and returns a SPECIFIC verdict (CertChainValidator).
    # LtvValidator and Pkcs7OpenSslBridge both route through these.
    TrustAnchorSet.cpp
    CertChainValidator.cpp
    LtvValidator.cpp
    # ValidationRequestDialog lives at src/ui/ per AC ("src/ui/ or
    # src/signatures/") but is compiled into pdfe_signatures to avoid a
    # cyclic dep — pdfe_ui already depends on pdfe_signatures, so the
    # ValidationCoordinator (in pdfe_signatures) cannot depend on
    # pdfe_ui without a cycle.
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/ValidationRequestDialog.cpp
)

target_include_directories(pdfe_signatures PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# GitHub #199 (M1a). OpenSSL 3 defines OPENSSL_malloc/OPENSSL_free as
# CRYPTO_malloc(num, OPENSSL_FILE, OPENSSL_LINE), and OPENSSL_FILE is __FILE__
# expanded in the CALLER's translation unit -- so every file here that allocates
# through those macros embeds its own absolute source path, build account name
# included, into the shipped binary.
#
# Measured: exactly 6 occurrences in pdfe.exe, and exactly the 6 files in this
# target that call those macros. The other OpenSSL-using files in the same
# target do not leak, and PDFE's own source contains no __FILE__ anywhere --
# which is what identifies the mechanism rather than guessing at it.
#
# OPENSSL_NO_FILENAMES makes those macros pass NULL instead. PRIVATE: it changes
# how this target's own calls expand and must not propagate to consumers.
#
# TRADE, stated rather than assumed: OpenSSL's error queue no longer records the
# file/line of an allocation failure from these six files. Nothing in this
# project reads that -- there is no ERR_get_error_line call anywhere -- and the
# alternative (#199's other route, a global __FILE__ rewrite) would reach code
# with no connection to this leak.
target_compile_definitions(pdfe_signatures PRIVATE OPENSSL_NO_FILENAMES)

target_link_libraries(pdfe_signatures PUBLIC
    pdfe_core
    Qt6::Widgets
    Qt6::Network
    OpenSSL::Crypto
    OpenSSL::SSL
)
if(WIN32)
    target_link_libraries(pdfe_signatures PUBLIC winhttp)
    message(STATUS "Signatures: Using direct OpenSSL PKCS7 implementation + WinHTTP TSA")
else()
    find_package(CURL REQUIRED)
    target_link_libraries(pdfe_signatures PUBLIC CURL::libcurl)
    message(STATUS "Signatures: Using direct OpenSSL PKCS7 implementation + libcurl TSA")
endif()

target_compile_features(pdfe_signatures PUBLIC cxx_std_20)
