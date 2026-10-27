// -*- coding: utf-8 -*-
// Phase 8b (core): FormEncoder + HostAllowlist. Network integration
// (QNetworkAccessManager + mock HTTPS server) is a follow-up phase —
// this commit locks down the pure pieces first.
#include <gtest/gtest.h>
#include "formsubmit/FormEncoder.h"
#include "formsubmit/HostAllowlist.h"

#include <QByteArray>
#include <string>
#include <vector>

using namespace pdfe::formsubmit;

// -------- Percent-encoding --------

TEST(FormEncodePercent, AlphaNumPassThrough)
{
    EXPECT_EQ(FormEncoder::percentEncodeFormValue("abc123"), "abc123");
    EXPECT_EQ(FormEncoder::percentEncodeFormValue("ABC"), "ABC");
}

TEST(FormEncodePercent, UnreservedPassThrough)
{
    EXPECT_EQ(FormEncoder::percentEncodeFormValue("-_.~"), "-_.~");
}

TEST(FormEncodePercent, SpaceBecomesPlus)
{
    EXPECT_EQ(FormEncoder::percentEncodeFormValue("hello world"),
              "hello+world");
}

TEST(FormEncodePercent, SpecialsPercentEncode)
{
    EXPECT_EQ(FormEncoder::percentEncodeFormValue("a&b=c"), "a%26b%3Dc");
    EXPECT_EQ(FormEncoder::percentEncodeFormValue("a/b?c"), "a%2Fb%3Fc");
}

TEST(FormEncodePercent, NonAsciiEncodedPerByte)
{
    // "é" = C3 A9 in UTF-8 → %C3%A9
    EXPECT_EQ(FormEncoder::percentEncodeFormValue("caf\xC3\xA9"),
              "caf%C3%A9");
}

// -------- URL-encoded format --------

TEST(FormEncodeUrl, JoinsFieldsWithAmpersand)
{
    std::vector<FormField> fields = {{"name", "Alice"},
                                     {"age",  "42"}};
    QByteArray out = FormEncoder::encode(fields, SubmitFormat::UrlEncoded);
    EXPECT_EQ(out, QByteArray("name=Alice&age=42"));
}

TEST(FormEncodeUrl, EscapesSpecialChars)
{
    std::vector<FormField> fields = {{"q", "hello world"},
                                     {"k", "a&b"}};
    QByteArray out = FormEncoder::encode(fields, SubmitFormat::UrlEncoded);
    EXPECT_EQ(out, QByteArray("q=hello+world&k=a%26b"));
}

TEST(FormEncodeUrl, EmptyFieldsProduceEmptyBody)
{
    QByteArray out = FormEncoder::encode({}, SubmitFormat::UrlEncoded);
    EXPECT_TRUE(out.isEmpty());
}

// -------- FDF format --------

TEST(FormEncodeFdf, HeaderAndEOFMarkersPresent)
{
    std::vector<FormField> fields = {{"a", "1"}};
    QByteArray out = FormEncoder::encode(fields, SubmitFormat::Fdf);
    EXPECT_TRUE(out.startsWith("%FDF-1.2"));
    EXPECT_TRUE(out.contains("%%EOF"));
    EXPECT_TRUE(out.contains("/FDF"));
    EXPECT_TRUE(out.contains("/Fields"));
}

TEST(FormEncodeFdf, EachFieldAppearsAsTVPair)
{
    std::vector<FormField> fields = {{"name", "Alice"},
                                     {"dept", "Eng"}};
    QByteArray out = FormEncoder::encode(fields, SubmitFormat::Fdf);
    EXPECT_TRUE(out.contains("/T (name)"));
    EXPECT_TRUE(out.contains("/V (Alice)"));
    EXPECT_TRUE(out.contains("/T (dept)"));
    EXPECT_TRUE(out.contains("/V (Eng)"));
}

// -------- XFDF format --------

TEST(FormEncodeXfdf, ProducesValidXmlShell)
{
    std::vector<FormField> fields = {{"f", "v"}};
    QByteArray out = FormEncoder::encode(fields, SubmitFormat::Xfdf);
    EXPECT_TRUE(out.startsWith("<?xml"));
    EXPECT_TRUE(out.contains("<xfdf"));
    EXPECT_TRUE(out.contains("</xfdf>"));
    EXPECT_TRUE(out.contains("<field name=\"f\">"));
    EXPECT_TRUE(out.contains("<value>v</value>"));
}

TEST(FormEncodeXfdf, XmlMetaCharsEscaped)
{
    std::vector<FormField> fields = {{"x", "a & <b>"}};
    QByteArray out = FormEncoder::encode(fields, SubmitFormat::Xfdf);
    EXPECT_TRUE(out.contains("a &amp; &lt;b&gt;"));
    EXPECT_FALSE(out.contains("a & <b>"));  // raw meta chars must not remain
}

// -------- Content types --------

TEST(FormEncodeMime, ReturnsAdobeDocumentedTypes)
{
    EXPECT_EQ(FormEncoder::contentType(SubmitFormat::UrlEncoded),
              "application/x-www-form-urlencoded");
    EXPECT_EQ(FormEncoder::contentType(SubmitFormat::Fdf),
              "application/vnd.fdf");
    EXPECT_EQ(FormEncoder::contentType(SubmitFormat::Xfdf),
              "application/vnd.adobe.xfdf");
}

// -------- HostAllowlist --------

TEST(HostAllowlist, StartsEmpty)
{
    HostAllowlist list;
    EXPECT_FALSE(list.contains("example.com"));
    EXPECT_TRUE(list.hosts().empty());
}

TEST(HostAllowlist, AllowAndContainsAreCaseInsensitive)
{
    HostAllowlist list;
    list.allow("Example.COM");
    EXPECT_TRUE(list.contains("example.com"));
    EXPECT_TRUE(list.contains("EXAMPLE.COM"));
    EXPECT_TRUE(list.contains("example.com."));  // trailing dot normalized
}

TEST(HostAllowlist, RevokeRemovesEntry)
{
    HostAllowlist list({"example.com", "other.example"});
    list.revoke("example.com");
    EXPECT_FALSE(list.contains("example.com"));
    EXPECT_TRUE(list.contains("other.example"));
}

TEST(HostAllowlist, PortStrippedFromLookup)
{
    HostAllowlist list({"example.com"});
    EXPECT_TRUE(list.contains("example.com:8443"));
}

TEST(HostAllowlist, NormalizeStripsCaseTrailingDotAndPort)
{
    EXPECT_EQ(HostAllowlist::normalize("Example.COM:443"), "example.com");
    EXPECT_EQ(HostAllowlist::normalize("EXAMPLE.COM."), "example.com");
}

TEST(HostAllowlist, HostsReturnsSortedLowercase)
{
    HostAllowlist list({"ZED.example", "alpha.example", "Beta.example"});
    const auto hosts = list.hosts();
    ASSERT_EQ(hosts.size(), 3u);
    EXPECT_EQ(hosts[0], "alpha.example");
    EXPECT_EQ(hosts[1], "beta.example");
    EXPECT_EQ(hosts[2], "zed.example");
}
