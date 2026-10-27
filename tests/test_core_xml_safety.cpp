// -*- coding: utf-8 -*-
//
// Unit tests for pdfe::core::xmlsafety::inspect / describe
// (src/core/XmlSafety.{h,cpp}) -- GAP A1-37, src/core dedicated coverage.
//
// WHY THIS FILE EXISTS ALONGSIDE test_xml_entity_safety.cpp
// --------------------------------------------------------
// test_xml_entity_safety.cpp is an END-TO-END file: it drives XMP, XFDF and
// hOCR through their real parsers and asserts the document is refused. It
// exercises the scanner only through two summary tests. The scanner itself is
// a hand-written pre-parse LEXER with comment/CDATA/quoted-literal skipping, a
// token-boundary rule, a two-way policy split and a four-way reason
// precedence -- i.e. exactly the kind of code whose edge cases an end-to-end
// test cannot reach. This file pins the lexer.
//
// ORACLES are the XML 1.0 (Fifth Edition) grammar and the CONTRACT written in
// XmlSafety.h, never current output:
//
//   * XML 1.0 sec. 2.5 -- comments run to the first "-->"; their content is
//     not markup.
//   * XML 1.0 sec. 2.7 -- CDATA runs to "]]>"; its content is character data.
//   * XML 1.0 sec. 2.8  / 4.2 -- markup declarations are `<!NAME ...>`, names
//     are CASE-SENSITIVE upper-case keywords, literals are quoted and MAY
//     contain '<' and '>'.
//   * XmlSafety.h -- Strict refuses ANY DTD construct; AllowDoctype (hOCR
//     only) permits "a bare/PUBLIC-identified DOCTYPE" and NOTHING else,
//     scanning still for "entity declarations, parameter-entity references,
//     and external identifiers anywhere OTHER than the DOCTYPE's own external
//     id".
//   * XmlSafety.h -- reason precedence is "most specific reason first":
//     ExternalIdentifier > EntityDeclaration > ParameterEntityRef > Doctype.
//   * XmlSafety.h -- "Never throws"; a truncated document "simply yields
//     whatever verdict the bytes seen so far justify".

#include <gtest/gtest.h>

#include "core/XmlSafety.h"

#include <pugixml.hpp>

#include <algorithm>
#include <ios>
#include <string>
#include <string_view>

using pdfe::core::xmlsafety::describe;
using pdfe::core::xmlsafety::doctypeExtent;
using pdfe::core::xmlsafety::Encoding;
using pdfe::core::xmlsafety::sniffEncoding;
using pdfe::core::xmlsafety::inspect;
using pdfe::core::xmlsafety::kEntityExpansionLimit;
using pdfe::core::xmlsafety::Policy;
using pdfe::core::xmlsafety::Reject;
using pdfe::core::xmlsafety::Verdict;

namespace {

constexpr Policy kStrict = Policy::Strict;
constexpr Policy kLenient = Policy::AllowDoctype;

// ---------------------------------------------------------------------------
// describe() / Verdict surface
// ---------------------------------------------------------------------------

// Every reason must have its own human-readable string: a log line saying
// "refused" without saying WHY is how a security control stops being
// actionable.
// KILLS: collapsing describe() to one string, or dropping a switch case (which
// would fall through to "unknown").
TEST(CoreXmlSafetyDescribe, EveryReasonHasADistinctNonPlaceholderString)
{
    const Reject all[] = {Reject::Ok, Reject::DoctypeDeclaration,
                          Reject::EntityDeclaration, Reject::ExternalIdentifier,
                          Reject::ParameterEntityRef,
                          Reject::EncodingNotUnderstood};

    for (Reject a : all) {
        const std::string sa = describe(a);
        EXPECT_FALSE(sa.empty());
        EXPECT_NE(sa, "unknown")
            << "reason " << static_cast<int>(a) << " has no describe() case";
        for (Reject b : all) {
            if (a == b) continue;
            EXPECT_NE(sa, std::string(describe(b)))
                << "reasons " << static_cast<int>(a) << " and "
                << static_cast<int>(b) << " share a description";
        }
    }
    EXPECT_STREQ(describe(Reject::Ok), "accepted");
}

// Verdict::safe() and operator bool must agree, and `detail` must be empty
// exactly when accepted (the header says so) so callers can use it directly as
// an exception message without emitting an empty reason.
// KILLS: populating `detail` unconditionally, or making operator bool the
// inverse of safe().
TEST(CoreXmlSafetyDescribe, DetailIsPopulatedExactlyWhenRejected)
{
    const Verdict ok = inspect("<root/>");
    EXPECT_TRUE(ok.safe());
    EXPECT_TRUE(static_cast<bool>(ok));
    EXPECT_EQ(ok.reason, Reject::Ok);
    EXPECT_TRUE(ok.detail.empty());

    const Verdict bad = inspect("<!DOCTYPE a><a/>");
    EXPECT_FALSE(bad.safe());
    EXPECT_FALSE(static_cast<bool>(bad));
    EXPECT_FALSE(bad.detail.empty())
        << "a rejection must carry an explanation for the log/exception";
}

// ---------------------------------------------------------------------------
// Strict policy: XMP (ISO 16684-1 sec. 7.1) and XFDF forbid a DTD outright
// ---------------------------------------------------------------------------

// KILLS: any narrowing of the strict arm (e.g. only refusing ENTITY, letting a
// bare DOCTYPE or an ELEMENT/ATTLIST/NOTATION declaration through).
TEST(CoreXmlSafetyStrict, RefusesEveryDtdConstruct)
{
    EXPECT_FALSE(inspect("<!DOCTYPE a><a/>", kStrict).safe());
    EXPECT_FALSE(inspect("<!DOCTYPE a SYSTEM \"x.dtd\"><a/>", kStrict).safe());
    EXPECT_FALSE(
        inspect("<!DOCTYPE a PUBLIC \"-//X//Y//EN\" \"x.dtd\"><a/>", kStrict)
            .safe());
    EXPECT_FALSE(inspect("<!DOCTYPE a [<!ENTITY e \"x\">]><a/>", kStrict).safe());
    EXPECT_FALSE(
        inspect("<!DOCTYPE a [<!ELEMENT a (#PCDATA)>]><a/>", kStrict).safe());
    EXPECT_FALSE(
        inspect("<!DOCTYPE a [<!ATTLIST a b CDATA #IMPLIED>]><a/>", kStrict)
            .safe());
    EXPECT_FALSE(
        inspect("<!DOCTYPE a [<!NOTATION n SYSTEM \"n\">]><a/>", kStrict)
            .safe());
}

// The classic XXE primitive and the classic expansion bomb, in their canonical
// shapes, must be refused with the RIGHT reason.
// KILLS: mapping every rejection onto a single generic reason.
TEST(CoreXmlSafetyStrict, ClassicAttackShapesGetTheirSpecificReason)
{
    // XXE / file disclosure.
    EXPECT_EQ(inspect("<!DOCTYPE r [<!ENTITY x SYSTEM \"file:///etc/passwd\">]>"
                      "<r>&x;</r>")
                  .reason,
              Reject::ExternalIdentifier);
    // Billion laughs -- internal entities only, no external id.
    EXPECT_EQ(inspect("<!DOCTYPE r [<!ENTITY a \"aa\"><!ENTITY b \"&a;&a;\">]>"
                      "<r>&b;</r>")
                  .reason,
              Reject::EntityDeclaration);
    // Parameter-entity reference inside a declaration.
    EXPECT_EQ(inspect("<!DOCTYPE r SYSTEM \"a\" %pe; ><r/>").reason,
              Reject::ExternalIdentifier);
}

// Header: "Most specific reason first: an external identifier is the SSRF /
// file disclosure primitive, an entity declaration is the expansion primitive,
// and a bare DTD is the enabling construct for both."
// KILLS: reordering the if/else-if chain in inspect().
TEST(CoreXmlSafetyStrict, ReasonPrecedenceIsMostSpecificFirst)
{
    // External id + entity decl + doctype all present -> ExternalIdentifier.
    EXPECT_EQ(inspect("<!DOCTYPE r [<!ENTITY x SYSTEM \"http://h/\">]><r/>")
                  .reason,
              Reject::ExternalIdentifier);
    // Entity decl + doctype, no external id -> EntityDeclaration.
    EXPECT_EQ(inspect("<!DOCTYPE r [<!ENTITY x \"y\">]><r/>").reason,
              Reject::EntityDeclaration);
    // Parameter ref + doctype, no entity decl, no external id -> ParameterEntityRef.
    EXPECT_EQ(inspect("<!DOCTYPE r %pe; ><r/>").reason,
              Reject::ParameterEntityRef);
    // Doctype alone -> DoctypeDeclaration.
    EXPECT_EQ(inspect("<!DOCTYPE r><r/>").reason, Reject::DoctypeDeclaration);
}

// ---------------------------------------------------------------------------
// Lenient policy (hOCR): a real Tesseract doctype, and nothing else
// ---------------------------------------------------------------------------

// Header: real hOCR carries the XHTML 1.0 Transitional doctype; refusing it
// would be "a functional regression dressed up as a security control".
// KILLS: deleting the Policy parameter / making hOCR strict again.
TEST(CoreXmlSafetyLenient, AcceptsTheRealTesseractXhtmlDoctype)
{
    const char* tesseract =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\""
        " \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
        "<div class='ocr_page' title='bbox 0 0 100 100'></div>"
        "</body></html>";
    EXPECT_TRUE(inspect(tesseract, kLenient).safe())
        << describe(inspect(tesseract, kLenient).reason);

    // A bare doctype with no external id is fine too.
    EXPECT_TRUE(inspect("<!DOCTYPE html><html/>", kLenient).safe());

    // ... but the SAME document is refused under Strict, because XMP/XFDF
    // specs forbid a DTD outright.
    EXPECT_EQ(inspect(tesseract, kStrict).reason, Reject::ExternalIdentifier);
    EXPECT_EQ(inspect("<!DOCTYPE html><html/>", kStrict).reason,
              Reject::DoctypeDeclaration);
}

// The lenient arm must still stop every primitive that matters. An external
// identifier on an ENTITY (as opposed to the DOCTYPE's own) is the XXE
// primitive itself and is refused under BOTH policies.
// KILLS: implementing AllowDoctype as "skip the whole scan when a doctype is
// present", or attributing an ENTITY's SYSTEM id to the doctype.
TEST(CoreXmlSafetyLenient, StillRefusesEntitiesExternalIdsAndOtherDeclarations)
{
    EXPECT_EQ(inspect("<!DOCTYPE html [<!ENTITY e \"x\">]><html/>", kLenient)
                  .reason,
              Reject::EntityDeclaration);
    EXPECT_EQ(inspect("<!DOCTYPE html [<!ENTITY e SYSTEM \"file:///x\">]>"
                      "<html/>",
                      kLenient)
                  .reason,
              Reject::ExternalIdentifier);
    EXPECT_EQ(inspect("<!DOCTYPE html [<!NOTATION n SYSTEM \"n\">]><html/>",
                      kLenient)
                  .reason,
              Reject::ExternalIdentifier);
    EXPECT_EQ(inspect("<!DOCTYPE html [<!ELEMENT p (#PCDATA)>]><html/>", kLenient)
                  .reason,
              Reject::DoctypeDeclaration);
    // An entity declaration WITHOUT any enclosing doctype (illegal XML, but the
    // scanner must not depend on the doctype to notice it).
    EXPECT_EQ(inspect("<!ENTITY e \"x\"><html/>", kLenient).reason,
              Reject::EntityDeclaration);
}

// A parameter-entity reference is listed in XmlSafety.h as one of "the
// primitives that actually matter" and is scanned for under BOTH policies.
// It must be caught wherever it appears in the DTD, including in the internal
// subset's own text -- the `%pe;` recall that pairs with an external parameter
// entity is the standard blind-XXE construction.
// KILLS: dropping the '%' branch; and (see report) any implementation that
// only inspects the text INSIDE a markup declaration.
TEST(CoreXmlSafetyLenient, RefusesParameterEntityReferencesAnywhereInTheDtd)
{
    // Inside the DOCTYPE declaration itself, before the internal subset.
    EXPECT_EQ(inspect("<!DOCTYPE html %pe; ><html/>", kLenient).reason,
              Reject::ParameterEntityRef);
    // In the internal subset's own text -- the classic `%pe;` recall.
    EXPECT_EQ(inspect("<!DOCTYPE html [ %pe; ]><html/>", kLenient).reason,
              Reject::ParameterEntityRef);
    EXPECT_EQ(inspect("<!DOCTYPE html [%remote;]><html/>", kLenient).reason,
              Reject::ParameterEntityRef);
    // The full blind-XXE construction: an external parameter entity plus its
    // recall. The external id is the more specific reason, so that is what is
    // reported -- but the recall alone (previous case) must already be enough.
    EXPECT_EQ(inspect("<!DOCTYPE r [<!ENTITY % p SYSTEM \"http://h/\"> %p; ]>"
                      "<r/>",
                      kLenient)
                  .reason,
              Reject::ExternalIdentifier);
}

// ---------------------------------------------------------------------------
// Lexer correctness -- comments, CDATA, quoted literals, token boundaries
// ---------------------------------------------------------------------------

// XML 1.0 sec. 2.5 / 2.7: comment and CDATA content is not markup. A guard
// that fired on them would refuse legitimate documents, and a guard that is
// known to cry wolf gets switched off.
// KILLS: removing the comment/CDATA skip.
TEST(CoreXmlSafetyLexer, CommentAndCdataContentIsNotMarkup)
{
    EXPECT_TRUE(inspect("<a><!-- <!DOCTYPE x> --></a>").safe());
    EXPECT_TRUE(inspect("<a><!-- <!ENTITY e SYSTEM 'file:///x'> --></a>").safe());
    EXPECT_TRUE(inspect("<a><![CDATA[<!ENTITY e \"x\">]]></a>").safe());
    EXPECT_TRUE(inspect("<a><![CDATA[<!DOCTYPE x SYSTEM 'y'>]]></a>").safe());
    // Several in a row, and markup resuming after them.
    EXPECT_TRUE(
        inspect("<a><!-- c1 --><![CDATA[%pe;]]><!-- c2 --><b/></a>").safe());
    // ... and the guard still fires on a real declaration that FOLLOWS them.
    EXPECT_EQ(inspect("<!-- lead --><!DOCTYPE a [<!ENTITY e \"x\">]><a/>").reason,
              Reject::EntityDeclaration);
}

// XML 1.0 sec. 2.8: a system/public literal is quoted and may contain '<' and
// '>'. If the scanner terminated the declaration at the first raw '>' it would
// resume mid-literal and mis-classify the remaining bytes.
// KILLS: removing the quoted-literal skip in the declaration walk.
TEST(CoreXmlSafetyLexer, QuotedLiteralsMayContainAngleBracketsAndPercent)
{
    // '>' inside the literal must not end the declaration early: the whole
    // thing is one DOCTYPE with its own external id.
    EXPECT_EQ(inspect("<!DOCTYPE a SYSTEM \"x>y<z.dtd\"><a/>", kStrict).reason,
              Reject::ExternalIdentifier);
    EXPECT_TRUE(inspect("<!DOCTYPE a PUBLIC 'a > b' 'c < d'><a/>", kLenient)
                    .safe())
        << "a PUBLIC id containing angle brackets is still just the DOCTYPE's "
           "own external id, which the lenient policy allows";

    // '%' inside a quoted literal is a literal percent sign, not a
    // parameter-entity reference: the declaration below is an ATTLIST, so the
    // reason must be DoctypeDeclaration, NOT ParameterEntityRef.
    EXPECT_EQ(inspect("<!DOCTYPE a [<!ATTLIST x y CDATA \"%notaref;\">]><a/>",
                      kLenient)
                  .reason,
              Reject::DoctypeDeclaration);

    // Both quote flavours.
    EXPECT_EQ(inspect("<!DOCTYPE a [<!ATTLIST x y CDATA '%alsonot;'>]><a/>",
                      kLenient)
                  .reason,
              Reject::DoctypeDeclaration);
}

// The SYSTEM/PUBLIC keywords are matched as whole tokens. Without that, any
// element or attribute value containing the substring would be refused.
// KILLS: replacing token_at() with a plain substring find.
TEST(CoreXmlSafetyLexer, ExternalIdKeywordsMatchAsWholeTokensOnly)
{
    // Substring look-alikes inside a declaration are NOT external ids: this
    // DOCTYPE has no external id, so lenient accepts it.
    EXPECT_TRUE(inspect("<!DOCTYPE MYSYSTEMID><a/>", kLenient).safe());
    EXPECT_TRUE(inspect("<!DOCTYPE a SYSTEMS><a/>", kLenient).safe());
    EXPECT_TRUE(inspect("<!DOCTYPE a PUBLICATION><a/>", kLenient).safe());
    EXPECT_TRUE(inspect("<!DOCTYPE a X-SYSTEM><a/>", kLenient).safe());

    // ... but the real keyword, delimited by whitespace, is.
    EXPECT_EQ(inspect("<!DOCTYPE a SYSTEM \"x\"><a/>", kLenient).reason,
              Reject::Ok)
        << "a DOCTYPE's OWN external id is allowed under the lenient policy";
    EXPECT_EQ(inspect("<!DOCTYPE a SYSTEM \"x\"><a/>", kStrict).reason,
              Reject::ExternalIdentifier);

    // Outside any declaration they are ordinary text/attribute content.
    EXPECT_TRUE(inspect("<a sysid=\"MYSYSTEMID\">SYSTEM PUBLIC</a>").safe());
    EXPECT_TRUE(inspect("<a>100% SYSTEM uptime; 50% off</a>").safe());
}

// A parameter-entity reference needs a name AND a terminating ';'. A bare '%'
// (a percent sign in an attribute or in text) must not trip the guard.
// KILLS: firing on '%' alone, which would refuse ordinary documents.
TEST(CoreXmlSafetyLexer, BarePercentIsNotAParameterEntityReference)
{
    EXPECT_TRUE(inspect("<a>50% off</a>").safe());
    EXPECT_TRUE(inspect("<a w='100%'/>").safe());
    // Inside a declaration: '%' with no name, and '%' with a name but no ';'.
    EXPECT_EQ(inspect("<!DOCTYPE a % ><a/>", kLenient).reason, Reject::Ok);
    EXPECT_EQ(inspect("<!DOCTYPE a %noterminator ><a/>", kLenient).reason,
              Reject::Ok);
    // ... and the complete form does fire.
    EXPECT_EQ(inspect("<!DOCTYPE a %yes; ><a/>", kLenient).reason,
              Reject::ParameterEntityRef);
}

// XML 1.0 sec. 2.8 makes markup-declaration keywords case-SENSITIVE, so
// `<!doctype` and `<!entity` are not the declarations they imitate. Whatever a
// downstream parser makes of them, the guard must FAIL CLOSED rather than wave
// an unrecognised `<!...>` construct through.
// KILLS: an "unknown declaration -> accept" default.
TEST(CoreXmlSafetyLexer, UnrecognisedMarkupDeclarationsFailClosed)
{
    EXPECT_FALSE(inspect("<!doctype html><html/>", kLenient).safe());
    EXPECT_FALSE(inspect("<!entity e \"x\"><a/>", kLenient).safe());
    EXPECT_FALSE(inspect("<!DocType a><a/>", kLenient).safe());
    EXPECT_FALSE(inspect("<!WHATEVER a><a/>", kLenient).safe());
    // The processing instruction and the XML declaration are NOT markup
    // declarations and must stay accepted.
    EXPECT_TRUE(inspect("<?xml version=\"1.0\"?><a/>").safe());
    EXPECT_TRUE(inspect("<?xpacket begin=\"\" id=\"W5M0\"?><a/>").safe());
}

// ---------------------------------------------------------------------------
// Benign real-world documents must sail through
// ---------------------------------------------------------------------------

// If the guard refused ordinary XMP or XFDF, metadata and form import would
// break -- and the guard would be reverted. This is the false-positive floor.
// KILLS: any over-broad heuristic (firing on '<!' inside text, on '%', on the
// literal words SYSTEM/PUBLIC, on an xpacket PI, or on RDF's many namespaces).
TEST(CoreXmlSafetyBenign, RealXmpAndXfdfDocumentsAreAccepted)
{
    const char* xmp =
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"PDFE\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\""
        " xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "   <dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">"
        "100% SYSTEM &amp; PUBLIC report &lt;draft&gt;</rdf:li></rdf:Alt>"
        "</dc:title>\n"
        "  </rdf:Description>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end=\"w\"?>";
    const Verdict vx = inspect(xmp, kStrict);
    EXPECT_TRUE(vx.safe()) << "benign XMP refused as: " << describe(vx.reason)
                           << " -- " << vx.detail;

    const char* xfdf =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n"
        " <fields>\n"
        "  <field name=\"discount\"><value>50%</value></field>\n"
        "  <field name=\"note\"><value>a &lt; b &amp; c &gt; d</value></field>\n"
        " </fields>\n"
        " <f href=\"form.pdf\"/>\n"
        "</xfdf>";
    const Verdict vf = inspect(xfdf, kStrict);
    EXPECT_TRUE(vf.safe()) << "benign XFDF refused as: " << describe(vf.reason)
                           << " -- " << vf.detail;
}

// ---------------------------------------------------------------------------
// Hostile / degenerate input -- "never throws"
// ---------------------------------------------------------------------------

// Header: "Never throws; a malformed/truncated document simply yields whatever
// verdict the bytes seen so far justify." Truncation is the cheapest way to
// walk a hand-written lexer off the end of its buffer.
// KILLS: an unguarded index/`substr` past the end, or a starts_with_at()
// whose `s.size() - pos` underflows.
TEST(CoreXmlSafetyHostile, TruncatedAndDegenerateInputIsHandledWithoutThrowing)
{
    const char* inputs[] = {
        "",      "<",    "<!",   "<!-",  "<!--", "<!--x", "<![",
        "<![C",  "<![CDATA[",    "<![CDATA[x",   "<!D",   "<!DOCTYPE",
        "<!DOCTYPE ",           "<!DOCTYPE a",  "<!DOCTYPE a [",
        "<!DOCTYPE a [<!ENTITY",                "<!ENTITY",
        "<!DOCTYPE a SYSTEM \"unterminated",    "%",     "%pe;",
        "<a>%pe;",              ">",    "<<<<",  "!!!",  "\"'\"'",
    };
    for (const char* in : inputs) {
        for (Policy p : {kStrict, kLenient}) {
            ASSERT_NO_THROW({
                const Verdict v = inspect(in, p);
                (void)v.safe();
                (void)describe(v.reason);
            }) << "input: [" << in << "]";
        }
    }

    // A truncated DOCTYPE has still DECLARED a doctype, so Strict refuses it.
    EXPECT_EQ(inspect("<!DOCTYPE", kStrict).reason, Reject::DoctypeDeclaration);
    // A truncated ENTITY declaration is still an entity declaration.
    EXPECT_EQ(inspect("<!ENTITY", kLenient).reason, Reject::EntityDeclaration);
    // An unterminated comment swallows the rest of the document as comment
    // text (XML 1.0 sec. 2.5), so nothing is declared.
    EXPECT_TRUE(inspect("<a><!-- <!ENTITY e \"x\">").safe());
    EXPECT_TRUE(inspect("<a><![CDATA[<!ENTITY e \"x\">").safe());
}

// Embedded NULs and control characters are illegal in XML 1.0 (sec. 2.2) and
// are a classic parser-differential trick. The scanner reads a string_view, so
// it must handle an interior NUL as data -- and must not silently ACCEPT a
// declaration that was split by one.
// KILLS: switching inspect() to a C-string / strlen-based scan (which would
// stop at the NUL and accept everything after it).
TEST(CoreXmlSafetyHostile, InteriorNulDoesNotEndTheScanOrLaunderADeclaration)
{
    // "<a/>\0<!DOCTYPE x [<!ENTITY e SYSTEM 'file:///x'>]>"
    const char raw[] =
        "<a/>\0<!DOCTYPE x [<!ENTITY e SYSTEM 'file:///x'>]>";
    const std::string_view sv(raw, sizeof(raw) - 1);
    EXPECT_EQ(inspect(sv, kLenient).reason, Reject::ExternalIdentifier)
        << "a declaration hidden behind an interior NUL must still be seen";

    // A NUL splitting the keyword itself makes it an unrecognised declaration,
    // which must fail closed rather than be accepted.
    const char split[] = "<!DOC\0TYPE a><a/>";
    EXPECT_FALSE(inspect(std::string_view(split, sizeof(split) - 1), kLenient)
                     .safe());
}

// Deep/repetitive input must not blow the stack or take super-linear time:
// inspect() is itself an attack surface (it runs before any parser).
// KILLS: rewriting the scan recursively, or an O(n^2) rescan of the tail.
TEST(CoreXmlSafetyHostile, LargeRepetitiveInputIsScannedLinearlyAndSafely)
{
    std::string manyLt(200000, '<');
    ASSERT_NO_THROW({ EXPECT_TRUE(inspect(manyLt).safe()); });

    std::string nestedComments;
    for (int i = 0; i < 20000; ++i) nestedComments += "<!-- x -->";
    nestedComments += "<a/>";
    ASSERT_NO_THROW({ EXPECT_TRUE(inspect(nestedComments).safe()); });

    // A huge quoted literal inside one declaration.
    std::string bigLiteral = "<!DOCTYPE a SYSTEM \"";
    bigLiteral.append(100000, 'x');
    bigLiteral += "\"><a/>";
    EXPECT_EQ(inspect(bigLiteral, kStrict).reason, Reject::ExternalIdentifier);

    // A long run of declarations, the last of which is the dangerous one.
    std::string many;
    for (int i = 0; i < 5000; ++i) many += "<!-- pad -->";
    many += "<!DOCTYPE a [<!ENTITY e \"x\">]><a/>";
    EXPECT_EQ(inspect(many).reason, Reject::EntityDeclaration);
}

// ---------------------------------------------------------------------------
// Encoding normalisation (meta-audit follow-up F27)
// ---------------------------------------------------------------------------
//
// ORACLE: XmlSafety.h's F27 contract — "sniffs the encoding the same way the
// parser does and transcodes UTF-16/UTF-32 input to UTF-8 before scanning",
// and pugixml's `guess_buffer_encoding` (BOM, else the NUL pattern of the
// first four bytes). The scan is an ASCII lexer; anything the PARSER can
// decode, the guard must decode too, or re-encoding the payload is a bypass.
// Measured pre-fix: UTF-16LE hOCR carrying `<!ENTITY leak SYSTEM ...>` was
// ACCEPTED and then parsed. See test_xml_entity_safety.cpp for the end-to-end
// measurement; this file pins the lexer-level behaviour.
//
// MUTATIONS KILLED (each applied to src/core/XmlSafety.cpp, confirmed RED,
// then reverted -- full table in docs/audit/meta-audit-2026-07/followup/
// phase-27-dispositions.md):
//   M1 sniff_encoding() returns Enc::Utf8 unconditionally (the whole F27
//      encoding fix reverted)              -> Utf16AndUtf32Payloads...,
//                                             NonAsciiAndSurrogatePairs...,
//                                             TruncatedAndOddLength... RED
//   M3 every non-UTF-8 document refused    -> BenignNonUtf8Documents... RED
//      (detection made UNIVERSAL, which is what proves it is SELECTIVE)
//   M5 BOM sniffed, no-BOM NUL-pattern     -> Utf16AndUtf32Payloads... RED
//      guess dropped (a partial revert)       (the BOM-less rows only)
//   M2 subset-scan comment/PI skips off    -> ParameterEntityRecall... RED
//   M4 subset '%' fires without a name+';' -> ParameterEntityRecall... RED
//      (its CONTROL arm)

namespace {

std::string encodeUtf16(std::string_view ascii, bool littleEndian, bool bom)
{
    std::string out;
    if (bom) {
        out += littleEndian ? '\xFF' : '\xFE';
        out += littleEndian ? '\xFE' : '\xFF';
    }
    for (char c : ascii) {
        if (littleEndian) { out += c; out += '\0'; }
        else              { out += '\0'; out += c; }
    }
    return out;
}

std::string encodeUtf32(std::string_view ascii, bool littleEndian, bool bom)
{
    std::string out;
    if (bom) {
        if (littleEndian) { out += '\xFF'; out += '\xFE'; out += '\0'; out += '\0'; }
        else              { out += '\0'; out += '\0'; out += '\xFE'; out += '\xFF'; }
    }
    for (char c : ascii) {
        if (littleEndian) { out += c; out += '\0'; out += '\0'; out += '\0'; }
        else              { out += '\0'; out += '\0'; out += '\0'; out += c; }
    }
    return out;
}

} // namespace

// KILLS: deleting the BOM/encoding sniff, or sniffing the BOM only (pugixml
// also guesses from the NUL pattern when no BOM is present).
TEST(CoreXmlSafetyEncoding, Utf16AndUtf32PayloadsAreNormalisedBeforeScanning)
{
    const char* hostile = "<!DOCTYPE r [<!ENTITY x SYSTEM \"http://h/\">]><r/>";
    const char* bomb    = "<!DOCTYPE r [<!ENTITY a \"aa\">]><r/>";
    const char* recall  = "<!DOCTYPE r [ %p; ]><r/>";

    struct Enc { const char* name; bool le; bool bom; bool utf32; };
    const Enc encs[] = {
        {"UTF-16LE + BOM",  true,  true,  false},
        {"UTF-16BE + BOM",  false, true,  false},
        {"UTF-16LE no BOM", true,  false, false},
        {"UTF-16BE no BOM", false, false, false},
        {"UTF-32LE + BOM",  true,  true,  true},
        {"UTF-32BE + BOM",  false, true,  true},
        {"UTF-32LE no BOM", true,  false, true},
        {"UTF-32BE no BOM", false, false, true},
    };

    for (const Enc& e : encs) {
        auto enc = [&](std::string_view s) {
            return e.utf32 ? encodeUtf32(s, e.le, e.bom)
                           : encodeUtf16(s, e.le, e.bom);
        };
        const std::string h = enc(hostile);
        // The premise: the ASCII literals the raw scan matches on are absent.
        ASSERT_EQ(h.find("ENTITY"), std::string::npos) << e.name;
        ASSERT_EQ(h.find("SYSTEM"), std::string::npos) << e.name;

        EXPECT_EQ(inspect(h, kLenient).reason, Reject::ExternalIdentifier)
            << e.name;
        EXPECT_EQ(inspect(enc(bomb), kLenient).reason, Reject::EntityDeclaration)
            << e.name;
        EXPECT_EQ(inspect(enc(recall), kLenient).reason,
                  Reject::ParameterEntityRef)
            << e.name;
        // ... and the reason precedence survives normalisation.
        EXPECT_EQ(inspect(enc("<!DOCTYPE r><r/>"), kStrict).reason,
                  Reject::DoctypeDeclaration)
            << e.name;
    }
}

// CONTROL for the test above. Normalisation must NOT become "refuse anything
// that is not UTF-8": XMP packets may legitimately be UTF-16 (ISO 16684-1),
// and refusing them would be a functional regression wearing a security
// control's badge.
// KILLS: implementing F27 as a blanket non-UTF-8 rejection.
TEST(CoreXmlSafetyEncoding, BenignNonUtf8DocumentsAreStillAccepted)
{
    const char* benign =
        "<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><v>100% SYSTEM report</v>"
        "</x:xmpmeta>";

    for (bool le : {true, false}) {
        for (bool bom : {true, false}) {
            const Verdict v16 = inspect(encodeUtf16(benign, le, bom), kStrict);
            EXPECT_TRUE(v16.safe())
                << "benign UTF-16 (le=" << le << " bom=" << bom
                << ") refused as: " << describe(v16.reason);
            const Verdict v32 = inspect(encodeUtf32(benign, le, bom), kStrict);
            EXPECT_TRUE(v32.safe())
                << "benign UTF-32 (le=" << le << " bom=" << bom
                << ") refused as: " << describe(v32.reason);
        }
    }

    // A UTF-8 BOM is not a UTF-16 BOM: the document behind it is still scanned
    // as UTF-8, both when benign and when hostile.
    EXPECT_TRUE(inspect(std::string("\xEF\xBB\xBF") + "<a/>", kStrict).safe());
    EXPECT_EQ(inspect(std::string("\xEF\xBB\xBF") +
                      "<!DOCTYPE r [<!ENTITY x SYSTEM \"h\">]><r/>", kStrict)
                  .reason,
              Reject::ExternalIdentifier);
}

// Non-ASCII content must survive normalisation as real UTF-8, so the lexer's
// token-boundary rule sees what the parser will. A transcoder that dropped or
// mangled non-ASCII could fuse "SYSTEM" onto a neighbouring name character (or
// split a name), changing the verdict.
// KILLS: transcoding by truncating each code unit to its low byte, or dropping
// code points above U+007F.
TEST(CoreXmlSafetyEncoding, NonAsciiAndSurrogatePairsSurviveNormalisation)
{
    // U+00E9 (e-acute) and U+1F600 (astral, a surrogate pair in UTF-16) inside
    // element text, next to an otherwise inert "SYSTEM" word.
    std::string ascii = "<a>caf\xC3\xA9 SYSTEM \xF0\x9F\x98\x80 100%</a>";
    // Re-encode by hand: build the UTF-16LE form of the same code points.
    const std::string utf16le = std::string("\xFF\xFE", 2) +
        std::string("<\0a\0>\0", 6) +
        std::string("c\0a\0f\0", 6) + std::string("\xE9\x00", 2) +
        std::string(" \0S\0Y\0S\0T\0E\0M\0 \0", 16) +
        std::string("\x3D\xD8\x00\xDE", 4) +      // U+1F600 surrogate pair
        std::string(" \0""1\0""0\0""0\0%\0", 10) +
        std::string("<\0/\0a\0>\0", 8);

    EXPECT_TRUE(inspect(ascii, kStrict).safe());
    const Verdict v = inspect(utf16le, kStrict);
    EXPECT_TRUE(v.safe()) << "benign non-ASCII UTF-16 refused as: "
                          << describe(v.reason);

    // The same bytes with a real declaration appended are still caught, so the
    // acceptance above is not "the transcoder gave up and produced nothing".
    const std::string hostile16 =
        utf16le + encodeUtf16("<!DOCTYPE r [<!ENTITY e \"x\">]>", true, false);
    EXPECT_EQ(inspect(hostile16, kStrict).reason, Reject::EntityDeclaration);
}

// The sniff runs before any indexing, so short and odd-length buffers are the
// cheapest way to walk it off the end. Header: "Never throws".
// KILLS: an unguarded p[3] in the no-BOM guess, or a UTF-16 loop that reads a
// trailing half code unit.
TEST(CoreXmlSafetyEncoding, TruncatedAndOddLengthNonUtf8BuffersAreHandledSafely)
{
    const std::string inputs[] = {
        std::string("\xFF", 1),
        std::string("\xFF\xFE", 2),
        std::string("\xFF\xFE<", 3),
        std::string("\xFE\xFF", 2),
        std::string("\xFF\xFE\x00", 3),
        std::string("\xFF\xFE\x00\x00", 4),
        std::string("\x00\x00\xFE\xFF", 4),
        std::string("\x00\x00\xFE\xFF<", 5),
        std::string("<\x00", 2),
        std::string("<\x00!\x00", 4),
        std::string("<\x00!\x00-", 5),
        std::string("\x00<\x00!", 4),
        std::string("<\x00\x00\x00", 4),
        std::string("\x00\x00\x00<", 4),
        // A lone high surrogate with no pair, and a lone low surrogate.
        std::string("\xFF\xFE\x3D\xD8", 4),
        std::string("\xFF\xFE\x00\xDE", 4),
    };
    for (const std::string& in : inputs) {
        for (Policy p : {kStrict, kLenient}) {
            ASSERT_NO_THROW({
                const Verdict v = inspect(in, p);
                (void)v.safe();
                (void)describe(v.reason);
            }) << "input length " << in.size();
        }
    }

    // A truncated UTF-16 declaration has still DECLARED a doctype.
    EXPECT_EQ(inspect(encodeUtf16("<!DOCTYPE", true, true), kStrict).reason,
              Reject::DoctypeDeclaration);
    // ... and an odd trailing byte does not launder the declaration before it.
    EXPECT_EQ(
        inspect(encodeUtf16("<!ENTITY e \"x\">", true, true) + std::string("\x41", 1),
                kLenient)
            .reason,
        Reject::EntityDeclaration);
}

// ---------------------------------------------------------------------------
// F27 REMEDIATION — the security-audit gate's findings
// ---------------------------------------------------------------------------

// FINDING 1 (the gate's FAIL). pugixml's two-byte fallback
// (pugixml.cpp:2046-2047) constrains bytes 2-3 not at all; the four-byte
// patterns above it additionally require the SECOND character to encode with a
// zero high byte, i.e. codepoint < 0x100. The first cut of F27 implemented only
// the four-byte patterns, so every document whose second character is Greek,
// Cyrillic, CJK, Hebrew or Arabic was scanned as UTF-8 while pugixml decoded
// the whole buffer as UTF-16. Measured pre-remediation: verdict `accepted`,
// pugixml status 0, root element decoded as `λ`.
// KILLS: removing the two-byte fallback from sniffEncoding().
TEST(CoreXmlSafetyEncoding, TwoByteFallbackCatchesANonLatinSecondCharacter)
{
    // Second characters that all defeat the four-byte pattern, one per script.
    const unsigned secondChars[] = {
        0x03BBu,   // GREEK SMALL LETTER LAMDA
        0x0416u,   // CYRILLIC CAPITAL LETTER ZHE
        0x4E2Du,   // CJK UNIFIED IDEOGRAPH-4E2D
        0x05D0u,   // HEBREW LETTER ALEF
        0x0627u,   // ARABIC LETTER ALEF
    };
    const char* dtd =
        "<!DOCTYPE evil [<!ENTITY xxe SYSTEM \"http://example.invalid/x\">]>";

    for (unsigned cp : secondChars) {
        for (bool le : {true, false}) {
            std::string doc;
            auto unit = [&](unsigned u) {
                if (le) { doc += char(u & 0xFF); doc += char((u >> 8) & 0xFF); }
                else    { doc += char((u >> 8) & 0xFF); doc += char(u & 0xFF); }
            };
            unit('<'); unit(cp); unit('/'); unit('>');
            for (char c : std::string(dtd)) unit(static_cast<unsigned char>(c));

            // The four-byte pattern demands a zero high byte on the second
            // character. Assert this document does NOT satisfy it, or the test
            // would pass against the very code the gate broke.
            const auto* b = reinterpret_cast<const unsigned char*>(doc.data());
            ASSERT_NE(b[le ? 2 : 3], 0x00u)
                << "U+" << std::hex << cp
                << " does not exercise the two-byte fallback";

            EXPECT_EQ(inspect(doc, kLenient).reason, Reject::ExternalIdentifier)
                << "U+" << std::hex << cp << (le ? " LE" : " BE")
                << ": scanned as UTF-8 while pugixml decodes it as UTF-16";
            EXPECT_EQ(sniffEncoding(doc),
                      le ? Encoding::Utf16LE : Encoding::Utf16BE);
        }
    }

    // CONTROL — broadening the sniff to a bare two-byte test must not steal a
    // legitimate UTF-8 document. It cannot: XML 1.0 sec. 2.2 excludes U+0000
    // from Char entirely, so no conforming UTF-8 XML begins `00 3C` or
    // `3C 00`. These benign UTF-8 documents must still be read as UTF-8.
    const char* benignUtf8[] = {
        "<r/>",
        "<?xml version=\"1.0\"?><r/>",
        "<\xCE\xBB>text</\xCE\xBB>",              // '<' then a UTF-8 lambda
        "<r>caf\xC3\xA9 100% SYSTEM PUBLIC</r>",
        "\xEF\xBB\xBF<r/>",                        // UTF-8 BOM
    };
    for (const char* d : benignUtf8) {
        EXPECT_EQ(sniffEncoding(d), Encoding::Utf8) << d;
        EXPECT_TRUE(inspect(d, kStrict).safe())
            << d << " refused as " << describe(inspect(d, kStrict).reason);
    }
    // ... and the UTF-8 forms of the hostile document are still refused, so the
    // fix did not move detection into an encoding-only special case.
    EXPECT_EQ(inspect(std::string("<r/>") + dtd, kLenient).reason,
              Reject::ExternalIdentifier);
    EXPECT_EQ(inspect(std::string("\xEF\xBB\xBF<r/>") + dtd, kLenient).reason,
              Reject::ExternalIdentifier);
}

// FINDING 2, MEASURED rather than reasoned. pugixml also detects `latin1` from
// the XML declaration (pugixml.cpp:2053-2069); sniffEncoding() has no
// equivalent and falls through to Utf8. The claim being tested is that this is
// SAFE, not that it is equivalent: latin1 and ASCII coincide over 0x00-0x7F, so
// no byte value can decode to a structural character it was not already, and
// every byte of every construct the scanner looks for is unchanged.
//
// This is measured against pugixml ITSELF over the whole byte range, not
// argued. If a future pugixml made latin1 decoding produce `<` from some other
// byte, this test reds.
// KILLS: an unmeasured assumption that latin1 needs no handling.
TEST(CoreXmlSafetyEncoding, Latin1IsByteCompatibleWithTheAsciiScan)
{
    // (a) latin1 detection is LIVE in this pugixml build -- otherwise the rest
    //     of this test would be vacuous.
    {
        const std::string doc =
            "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?><r>caf\xE9</r>";
        pugi::xml_document d;
        const pugi::xml_parse_result res =
            d.load_buffer(doc.data(), doc.size(), pugi::parse_default);
        ASSERT_TRUE(res) << "latin1 document must parse";
        ASSERT_EQ(res.encoding, pugi::encoding_latin1)
            << "this pugixml does not detect latin1; the measurement below "
               "would prove nothing";
        // 0xE9 became U+00E9, i.e. UTF-8 C3 A9 -- a real transcode happened.
        EXPECT_STREQ(d.child("r").text().as_string(), "caf\xC3\xA9");
    }

    // (b) THE MEASUREMENT: no byte value, decoded as latin1 by pugixml,
    //     produces a character the scanner treats as structural unless it was
    //     already that byte.
    static const std::string kStructural = "<>&%;'\"[]!-?/";
    int manufactured = 0;
    int measured = 0;
    for (int b = 1; b < 256; ++b) {
        if (kStructural.find(static_cast<char>(b)) != std::string::npos) continue;
        std::string doc = "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?><r>";
        doc += static_cast<char>(b);
        doc += "</r>";
        pugi::xml_document d;
        if (!d.load_buffer(doc.data(), doc.size(), pugi::parse_default)) continue;
        ++measured;
        const std::string text = d.child("r").text().as_string();
        if (text.find_first_of(kStructural) != std::string::npos) {
            ++manufactured;
            ADD_FAILURE() << "latin1 decoding of byte 0x" << std::hex << b
                          << " produced a structural character; the UTF-8 scan "
                             "would not see it";
        }
    }
    EXPECT_EQ(manufactured, 0);
    EXPECT_GE(measured, 200)
        << "the byte sweep must actually have exercised most of the range";

    // (c) the consequence: a latin1-declared hostile document is refused, and a
    //     latin1-declared benign one is accepted. Same scan, no latin1 branch.
    const char* hostile =
        "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>"
        "<!DOCTYPE r [<!ENTITY x SYSTEM \"http://h/\">]><r>caf\xE9</r>";
    const char* hostileAlias =
        "<?xml version=\"1.0\" encoding=\"latin1\"?>"
        "<!DOCTYPE r [<!ENTITY x SYSTEM \"http://h/\">]><r>caf\xE9</r>";
    EXPECT_EQ(inspect(hostile, kLenient).reason, Reject::ExternalIdentifier);
    EXPECT_EQ(inspect(hostileAlias, kLenient).reason, Reject::ExternalIdentifier);
    const char* benign =
        "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>"
        "<r>caf\xE9 100% SYSTEM PUBLIC</r>";
    EXPECT_TRUE(inspect(benign, kStrict).safe())
        << describe(inspect(benign, kStrict).reason);
    // A latin1 document is classified Utf8 by the sniff, deliberately.
    EXPECT_EQ(sniffEncoding(hostile), Encoding::Utf8);
}

// The short-buffer divergence, confirmed and recorded rather than left
// unexamined. pugixml abandons autodetection entirely below four bytes
// (pugixml.cpp:2027) and calls even a BOM'd buffer UTF-8; sniffEncoding()
// honours a two-byte BOM. PDFE is therefore STRICTER, which is the safe
// direction — and no DTD construct fits in fewer than four bytes, so neither
// reading can carry a declaration and the verdicts agree regardless.
// KILLS: "fixing" the divergence by dropping the n>=2 BOM tests, and any change
// that makes PDFE LOOSER than the parser here.
TEST(CoreXmlSafetyEncoding, ShortBuffersAreStricterThanPugixmlAndSayWhy)
{
    // No hand-typed expectation column. The classification PDFE picks for a
    // short buffer is an implementation detail of the floor effect; what must
    // hold is the PROPERTY. (Writing the column out by hand is what produced
    // the gate's finding in the first place, and it got this test's first cut
    // wrong too: `3C 00` at two bytes reaches the two-byte fallback and is
    // classified UTF-16LE, which the typed table did not predict.)
    const std::string shortBuffers[] = {
        std::string("\xFF\xFE", 2),      // UTF-16LE BOM, truncated
        std::string("\xFF\xFE<", 3),
        std::string("\xFE\xFF", 2),      // UTF-16BE BOM, truncated
        std::string("\xFE\xFF\x00", 3),
        std::string("<", 1),
        std::string("<\x00", 2),         // reaches the two-byte fallback
        std::string("\x00<", 2),
        std::string("\xEF\xBB", 2),      // UTF-8 BOM, truncated
        std::string("", 0),
    };

    for (const std::string& bytes : shortBuffers) {
        ASSERT_LT(bytes.size(), 4u);

        // (a) pugixml really does abandon autodetection here. If this ever
        //     stops being true the divergence below needs re-reading, and this
        //     assertion is what will say so.
        pugi::xml_document d;
        const pugi::xml_parse_result res =
            d.load_buffer(bytes.data(), bytes.size(), pugi::parse_default);
        EXPECT_EQ(res.encoding, pugi::encoding_utf8)
            << "pugixml skips autodetection below 4 bytes (pugixml.cpp:2027); "
               "buffer of " << bytes.size() << " bytes";

        // (b) PDFE may classify it as something else — that is the divergence,
        //     and it is in the STRICT direction: PDFE decodes where the parser
        //     does not, so it can only see MORE, never less.
        // (c) ... and it changes no verdict, because no DTD construct fits in
        //     fewer than four bytes. Both readings accept.
        for (Policy p : {kStrict, kLenient}) {
            ASSERT_NO_THROW({
                const Verdict v = inspect(bytes, p);
                EXPECT_TRUE(v.safe())
                    << "a sub-4-byte buffer cannot declare anything, so it must "
                       "not be refused: " << describe(v.reason);
            });
        }
    }

    // The shortest input that CAN carry a markup declaration is 4 bytes
    // (`<!a>`), which is exactly where pugixml starts autodetecting — so the
    // divergence cannot hide a declaration from either side.
    EXPECT_EQ(inspect("<!a>", kLenient).reason, Reject::DoctypeDeclaration)
        << "the 4-byte floor is also the shortest possible declaration";

    // The first buffer that is long enough for pugixml to autodetect must agree
    // exactly — the divergence is a floor effect, not a policy difference.
    const std::string fourByte("\xFF\xFE<\x00", 4);
    EXPECT_EQ(sniffEncoding(fourByte), Encoding::Utf16LE);
    pugi::xml_document d;
    EXPECT_EQ(d.load_buffer(fourByte.data(), fourByte.size(), pugi::parse_default)
                  .encoding,
              pugi::encoding_utf16_le);
}

// ---------------------------------------------------------------------------
// Internal-subset scan: comments and processing instructions (F27)
// ---------------------------------------------------------------------------

// XML 1.0 sec. 2.5 / 2.6: a comment ends at "-->" and a PI at "?>", NOT at the
// first '>'. The internal-subset scan used to step over any nested `<...>` by
// running to the first unquoted '>', so a '>' inside a comment ended the step
// early and a ']' later in the SAME comment then ended the whole subset scan —
// hiding the `%pe;` that followed. Measured pre-fix:
//   <!DOCTYPE html [ <!-- > ] --> %pe; ]>  ->  accepted
// KILLS: reverting the comment/PI handling in the subset scan.
TEST(CoreXmlSafetySubset, ParameterEntityRecallSurvivesCommentsAndPisInTheSubset)
{
    const char* hostile[] = {
        "<!DOCTYPE html [ %pe; ]><html/>",
        "<!DOCTYPE html [ <!-- > ] --> %pe; ]><html/>",
        "<!DOCTYPE html [ <?pi > ] ?> %pe; ]><html/>",
        "<!DOCTYPE html [ <!-- ]> --> %pe; ]><html/>",
        "<!DOCTYPE html [ <![INCLUDE[ %pe; ]]> ]><html/>",
        "<!DOCTYPE html [ <!-- a --><!-- > ] --> %pe; ]><html/>",
        "<!DOCTYPE html [ <?p1 > ] ?><!-- > ] --> %pe; ]><html/>",
        // An unterminated comment swallows the rest, so nothing is declared —
        // but the DOCTYPE itself is still there under Strict (below).
    };
    for (const char* h : hostile) {
        EXPECT_EQ(inspect(h, kLenient).reason, Reject::ParameterEntityRef)
            << "unflagged recall in: " << h;
    }

    // CONTROL — the guard must not fire merely because a comment or PI appears
    // in the subset, and '%' still needs a name and a ';'.
    EXPECT_TRUE(inspect("<!DOCTYPE html [ <!-- just a comment --> ]><html/>",
                        kLenient).safe());
    EXPECT_TRUE(inspect("<!DOCTYPE html [ <!-- > ] --> ]><html/>", kLenient).safe());
    EXPECT_TRUE(inspect("<!DOCTYPE html [ <?pi > ] ?> ]><html/>", kLenient).safe());
    EXPECT_TRUE(inspect("<!DOCTYPE html [ <!-- > ] --> % ]><html/>", kLenient).safe());
    EXPECT_TRUE(
        inspect("<!DOCTYPE html [ <!-- > ] --> %noterm ]><html/>", kLenient).safe());
    // A `%pe;` inside a comment is comment TEXT, not a reference.
    EXPECT_TRUE(inspect("<!DOCTYPE html [ <!-- %pe; --> ]><html/>", kLenient).safe());
    // A declaration following the comment is still found by the outer scan.
    EXPECT_EQ(inspect("<!DOCTYPE html [ <!-- > ] --> <!ENTITY e \"x\"> ]><html/>",
                      kLenient)
                  .reason,
              Reject::EntityDeclaration);
    // An unterminated comment in the subset swallows the rest (sec. 2.5).
    EXPECT_TRUE(inspect("<!DOCTYPE html [ <!-- %pe; ]><html/>", kLenient).safe());
    EXPECT_EQ(inspect("<!DOCTYPE html [ <!-- %pe; ]><html/>", kStrict).reason,
              Reject::DoctypeDeclaration);
}

// ---------------------------------------------------------------------------
// CDATA is context-dependent: inert in element content, markup in a DTD (F27
// remediation 2, the security-audit gate's second finding)
// ---------------------------------------------------------------------------

// ORACLE: XML 1.0 sec. 2.7 (a CDATA section's content is character data) vs
// sec. 2.8 (the internal subset admits markupdecl | PEReference | comment | PI
// | conditional section — a CDATA section is none of these, so it cannot
// legally appear there at all). The scanner applied the sec. 2.7 rule
// unconditionally; measured pre-fix,
//   `<!DOCTYPE html [ <![CDATA[<!ENTITY xxe SYSTEM "http://h/">]]> ]>`
// was ACCEPTED and pugixml parsed it.
// KILLS: reverting the `lt >= subsetEnd` context guard.
TEST(CoreXmlSafetyCdataContext, CdataIsMarkupInTheSubsetAndTextInElementContent)
{
    // (a) In the subset: scanned. Every marked-section keyword, and the two
    //     bare primitives.
    for (const char* kw : {"CDATA", "INCLUDE", "IGNORE", "WHATEVER"}) {
        const std::string ext = std::string("<!DOCTYPE html [ <![") + kw +
                                "[<!ENTITY x SYSTEM \"http://h/\">]]> ]><html/>";
        EXPECT_EQ(inspect(ext, kLenient).reason, Reject::ExternalIdentifier) << kw;
        const std::string ent = std::string("<!DOCTYPE html [ <![") + kw +
                                "[<!ENTITY x \"y\">]]> ]><html/>";
        EXPECT_EQ(inspect(ent, kLenient).reason, Reject::EntityDeclaration) << kw;
        const std::string pe = std::string("<!DOCTYPE html [ <![") + kw +
                               "[ %pe; ]]> ]><html/>";
        EXPECT_EQ(inspect(pe, kLenient).reason, Reject::ParameterEntityRef) << kw;
    }

    // (b) In ELEMENT CONTENT: inert, in every arrangement that could confuse
    //     the subset tracking. THIS IS THE DISCRIMINATING CONTROL.
    const char* inert[] = {
        "<a><![CDATA[<!ENTITY e SYSTEM \"http://h/\">]]></a>",
        "<a><![CDATA[<!DOCTYPE x [ <!ENTITY e SYSTEM 'h'> ]>]]></a>",
        "<a><![CDATA[%pe;]]></a>",
        // ... after a CLOSED subset. The subset must have ENDED at its ']'.
        "<!DOCTYPE html [ ]><a><![CDATA[<!ENTITY e SYSTEM \"h\">]]></a>",
        "<!DOCTYPE html [ <!-- c --> ]><a><![CDATA[<!ENTITY e SYSTEM \"h\">]]></a>",
        // Several CDATA sections in a row.
        "<!DOCTYPE html [ ]><a><![CDATA[<!ENTITY a SYSTEM \"h\">]]>"
        "<b/><![CDATA[<!ENTITY b SYSTEM \"h\">]]></a>",
    };
    for (const char* d : inert) {
        const Verdict v = inspect(d, kLenient);
        EXPECT_TRUE(v.safe())
            << "element-content CDATA is character data and must stay inert; "
               "refused as " << describe(v.reason) << " -- " << d;
    }

    // (c) THE SHARPEST CONTROL. These documents ARE refused — but for a
    //     declaration in their subset, never for the element-content CDATA
    //     that follows it. The reason must stay `DoctypeDeclaration`; if the
    //     trailing CDATA were being scanned, the "most specific reason first"
    //     precedence would escalate it to ExternalIdentifier. A blanket
    //     "scan CDATA everywhere" fix passes (b) trivially by never reaching
    //     those documents, but cannot pass this.
    const char* refusedForSomethingElse[] = {
        // A ']' inside a nested declaration's literal — subset ends at the
        // REAL ']', so the CDATA after it is element content.
        "<!DOCTYPE html [ <!ATTLIST x y CDATA \"]\"> ]>"
        "<a><![CDATA[<!ENTITY e SYSTEM \"h\">]]></a>",
        // A marked section in the subset, then element-content CDATA.
        "<!DOCTYPE html [ <![IGNORE[ <!ELEMENT z (#PCDATA)> ]]> ]>"
        "<a><![CDATA[<!ENTITY e SYSTEM \"h\">]]></a>",
    };
    for (const char* d : refusedForSomethingElse) {
        const Verdict v = inspect(d, kLenient);
        EXPECT_EQ(v.reason, Reject::DoctypeDeclaration)
            << "refused for the wrong thing — the trailing CDATA is element "
               "content and must not have been scanned: " << d;
    }

    // ... and the strict policy refuses the DOCTYPE itself, again not the
    // CDATA: the reason must not escalate to EntityDeclaration.
    EXPECT_EQ(inspect("<!DOCTYPE html [ ]><a><![CDATA[<!ENTITY e \"x\">]]></a>",
                      kStrict).reason,
              Reject::DoctypeDeclaration);
}

// The subset-extent tracking must fail toward SCANNING, never toward skipping.
// KILLS: computing the subset end with a plain find(']'), which a ']' inside a
// nested declaration's literal would cut short; and letting a nested marked
// section's shorter extent overwrite the enclosing subset's.
TEST(CoreXmlSafetyCdataContext, SubsetExtentIsRobustToUnterminationNestingAndLiterals)
{
    // Unterminated subset: everything after the '[' is still DTD.
    EXPECT_EQ(inspect("<!DOCTYPE html [ <![CDATA[<!ENTITY x SYSTEM \"h\">]]>",
                      kLenient).reason,
              Reject::ExternalIdentifier);
    EXPECT_EQ(inspect("<!DOCTYPE html [ <![CDATA[ %pe; ]]>", kLenient).reason,
              Reject::ParameterEntityRef);

    // A ']' inside a nested declaration's quoted literal must not end it.
    EXPECT_EQ(inspect("<!DOCTYPE html [ <!ATTLIST x y CDATA \"]\"> "
                      "<![CDATA[<!ENTITY e SYSTEM \"h\">]]> ]><html/>",
                      kLenient).reason,
              Reject::ExternalIdentifier);
    EXPECT_EQ(inspect("<!DOCTYPE html [ <!ATTLIST x y CDATA ']'> "
                      "<![CDATA[<!ENTITY e SYSTEM \"h\">]]> ]><html/>",
                      kLenient).reason,
              Reject::ExternalIdentifier);

    // A ']]>' inside a nested declaration's literal must not end a marked
    // section early and hand the rest back to the element-content rules.
    EXPECT_EQ(inspect("<!DOCTYPE html [ <![INCLUDE[ <!ENTITY a \"]]>\"> ]]> "
                      "<![CDATA[<!ENTITY e SYSTEM \"h\">]]> ]><html/>",
                      kLenient).reason,
              Reject::ExternalIdentifier);

    // A marked section NESTED in the subset must not shrink the tracked
    // extent: the CDATA that follows it is still inside the outer subset.
    EXPECT_EQ(inspect("<!DOCTYPE html [ <![IGNORE[ x ]]> "
                      "<![CDATA[<!ENTITY e SYSTEM \"h\">]]> ]><html/>",
                      kLenient).reason,
              Reject::ExternalIdentifier);
    EXPECT_EQ(inspect("<!DOCTYPE html [ <![IGNORE[ x ]]> <![IGNORE[ y ]]> "
                      "<![CDATA[<!ENTITY e SYSTEM \"h\">]]> ]><html/>",
                      kLenient).reason,
              Reject::ExternalIdentifier);

    // A comment UNTERMINATED to EOF inside the subset swallows the remainder
    // as comment text (XML 1.0 sec. 2.5) — so nothing is declared, and that is
    // correct rather than a hole: pugixml rejects such a document outright, so
    // there is no parse for a declaration to reach. Asserted, not assumed.
    {
        const char* doc = "<!DOCTYPE html [ <!-- <!ENTITY e SYSTEM \"h\"> ]><html/>";
        EXPECT_TRUE(inspect(doc, kLenient).safe());
        pugi::xml_document d;
        const pugi::xml_parse_result res =
            d.load_buffer(doc, std::char_traits<char>::length(doc),
                          pugi::parse_default);
        EXPECT_FALSE(res)
            << "an unterminated comment must not parse; if pugixml ever "
               "accepted one, this acceptance would become a real hole";
    }
    // ... and the same document WITH the comment closed is refused.
    EXPECT_EQ(inspect("<!DOCTYPE html [ <!-- c --> <!ENTITY e SYSTEM \"h\"> ]>"
                      "<html/>", kLenient).reason,
              Reject::ExternalIdentifier);

    // An unterminated marked section likewise extends the subset to EOF, so
    // what follows is scanned rather than skipped.
    EXPECT_EQ(inspect("<!DOCTYPE html [ <![CDATA[ <!ENTITY e SYSTEM \"h\">",
                      kLenient).reason,
              Reject::ExternalIdentifier);
}

// ---------------------------------------------------------------------------
// DOCTYPE structure: depth counting, and the two DELIBERATE divergences
// (F27 remediation 3+4 — the security-audit gate's third and fourth findings)
// ---------------------------------------------------------------------------

// ORACLE: pugixml's `parse_doctype_ignore` (:3017-3046) and
// `parse_doctype_group` (:3048-3090), both of which DEPTH-COUNT. The
// hand-written steppers they replaced stopped at the first `]]>` and the first
// unquoted `>` respectively, which ended the DTD early and let the
// element-content CDATA rule re-engage inside it.
// KILLS: removing the depth counter from either port.
TEST(CoreXmlSafetyDoctypeStructure, NestedSectionsAndGroupsAreDepthCounted)
{
    // Marked sections, nested. Only depth counting reaches the outer `]]>`.
    EXPECT_EQ(inspect("<!DOCTYPE html [ <![INCLUDE[ <![IGNORE[ x ]]> ]]> "
                      "<![CDATA[<!ENTITY e SYSTEM \"h\">]]> ]><html/>",
                      kLenient).reason,
              Reject::ExternalIdentifier);
    EXPECT_EQ(inspect("<!DOCTYPE html [ <![A[ <![B[ <![C[ x ]]> ]]> ]]> "
                      "<![CDATA[<!ENTITY e SYSTEM \"h\">]]> ]><html/>",
                      kLenient).reason,
              Reject::ExternalIdentifier);
    // Control groups, nested. A NON-ALPHABETIC name sets no flag of its own, so
    // this isolates the extent defect from any incidental refusal.
    EXPECT_EQ(inspect("<!DOCTYPE html [ <!1 <!2> ] > "
                      "<![CDATA[<!ENTITY e SYSTEM \"h\">]]> ]><html/>",
                      kLenient).reason,
              Reject::ExternalIdentifier);
    EXPECT_EQ(inspect("<!DOCTYPE html [ <!1 <!2 <!3> > ] > "
                      "<![CDATA[<!ENTITY e SYSTEM \"h\">]]> ]><html/>",
                      kLenient).reason,
              Reject::ExternalIdentifier);

    // The extent itself, checked directly.
    {
        const std::string d = "<!DOCTYPE html [ <![INCLUDE[ <![IGNORE[ x ]]> ]]> ]>";
        EXPECT_EQ(sniffEncoding(d), Encoding::Utf8);
        EXPECT_EQ(doctypeExtent(d, 0), d.size())
            << "the whole declaration is one group";
    }
    {
        const std::string d = "<!DOCTYPE html [ <!1 <!2> ] > ]>";
        EXPECT_EQ(doctypeExtent(d, 0), d.size());
    }

    // Unterminated nesting extends to EOF — never a fallback to the first
    // terminator, which would truncate the DTD.
    for (const char* u : {"<!DOCTYPE html [ <![INCLUDE[ <![IGNORE[ x ]]> ",
                          "<!DOCTYPE html [ <!1 <!2 ",
                          "<!DOCTYPE html [ <![A[ <![B[ "}) {
        const std::string d(u);
        EXPECT_EQ(doctypeExtent(d, 0), d.size()) << u;
    }

    // CONTROL — a single, properly closed section still ends where it should,
    // so depth counting has not made every section run to EOF.
    {
        const std::string d = "<!DOCTYPE html [ <![IGNORE[ x ]]> ]>";
        EXPECT_EQ(doctypeExtent(d, 0), d.size());
        EXPECT_TRUE(inspect(d + "<html/>", kLenient).safe());
        // ... and element content after it is element content.
        EXPECT_TRUE(inspect(d + "<a><![CDATA[<!ENTITY e SYSTEM \"h\">]]></a>",
                            kLenient).safe());
    }
}

// DIVERGENCE 1, DECIDED AND NOW PINNED. pugixml's `parse_doctype_ignore` does
// NO quote tracking, so a `]]>` inside a quoted literal ends the section for
// it. An earlier cut of this code tracked quotes there; it is now matched to
// pugixml, and THIS test is what holds it matched.
//
// THE PREMISE THIS TEST REPLACES WAS WRONG, AND THE ERROR IS THE POINT.
// It previously asserted that every document distinguishing the two readings
// is one pugixml REFUSES -- therefore no verdict could depend on the choice,
// therefore no test could kill a mutation reintroducing quote tracking. That
// generalisation came from sampling BALANCED-quote documents, all of which
// pugixml does refuse: once the section ends inside the literal, the leftover
// quote opens a primitive that never closes and parse_doctype_primitive
// throws.
//
// The F27 security-audit gate disproved it with an ODD number of quotes. The
// unbalanced trailing quote is consumed as the *closing* half of a later
// primitive, which leaves pugixml a parseable document while the two readings
// still disagree:
//
//     <!DOCTYPE html [ <![X[ "]]>" y" ]><r/>
//       pugixml:              accepted, doctype ends at 34
//       no tracking (ours):   34   <- matches
//       tracking (mutant):    38   <- diverges
//
// So the class of document does exist, PDFE's behaviour was right all along,
// and only the justification was fabricated. Same shape as the four findings
// before it: a claim about a hand-mirrored parser routine asserted rather than
// measured -- written, this time, in the paragraph that was fixing the
// previous four.
//
// DIRECTION OF THE DIVERGENCE, REASONED RATHER THAN ASSERTED.
// Quote tracking can only cause the section scan to SKIP a region. Skipping
// moves the cursor forward past candidate `]]>` terminators; it can never move
// it back. So the section end under tracking is always >= the end without it,
// hence doctype_group resumes later, hence the extent is >= too. Measured on
// the document above: 38 vs 34, and 38 is the whole buffer, i.e. the fail-long
// value.
//
// A LONGER extent is the safe direction against a BYPASS -- more bytes scanned
// as DTD. But it is the unsafe direction for FALSE POSITIVES: element content
// the parser treats as element content would be scanned as DTD, so a
// legitimate `<![CDATA[ ... ]]>` quoting markup as text after the DOCTYPE
// would be refused. XmlSafety.h names that failure mode explicitly -- "a
// functional regression wearing a security control's badge, which is how
// security controls get switched off". The third block below asserts exactly
// that consequence, so the mutation is killed by a *functional* regression as
// well as by the extent mismatch.
//
// KILLS: reintroducing quote tracking in doctype_ignore().
TEST(CoreXmlSafetyDoctypeStructure, MarkedSectionQuoteHandlingMatchesPugixmlOnDocumentsThatDistinguishThem)
{
    // pugixml's own reported DOCTYPE extent, read back from a parse_doctype
    // node's value (pugixml.cpp:3186-3193), never recomputed here.
    auto pugiExtent = [](const std::string& doc) -> std::size_t {
        pugi::xml_document d;
        const pugi::xml_parse_result res = d.load_buffer(
            doc.data(), doc.size(), pugi::parse_default | pugi::parse_doctype);
        if (!res) return std::string::npos;
        for (pugi::xml_node c = d.first_child(); c; c = c.next_sibling()) {
            if (c.type() == pugi::node_doctype) {
                std::size_t mark = 9;  // past "<!DOCTYPE"
                while (mark < doc.size() &&
                       (doc[mark] == ' ' || doc[mark] == '\t' ||
                        doc[mark] == '\r' || doc[mark] == '\n')) {
                    ++mark;
                }
                return mark + std::char_traits<char>::length(c.value()) + 1;
            }
        }
        return std::string::npos;
    };

    // (a) DISCRIMINATING **AND ACCEPTED**. These are the documents whose
    //     existence the old premise denied. Each must parse, and PDFE's extent
    //     must equal pugixml's -- which it can only do without quote tracking.
    const char* discriminating[] = {
        "<!DOCTYPE html [ <![X[ \"]]>\" y\" ]><r/>",
        "<!DOCTYPE html [ <![X[ \"]]>\" a\"b ]><r/>",
        "<!DOCTYPE html [ <![X[ ']]>' y' ]><r/>",
        "<!DOCTYPE html [ <![INCLUDE[ \"]]>\" z\" ]><r/>",
    };
    for (const char* c : discriminating) {
        const std::string doc(c);
        const std::size_t theirs = pugiExtent(doc);
        ASSERT_NE(theirs, std::string::npos)
            << "this document must be ACCEPTED by pugixml, or it cannot pin "
               "anything -- the old premise was that no such document exists: "
            << c;
        EXPECT_EQ(doctypeExtent(doc, 0), theirs)
            << "doctypeExtent disagrees with pugixml on a document the parser "
               "accepts. Quote tracking in doctype_ignore() produces exactly "
               "this mismatch: " << c;
        // Pin the shape too: an ODD number of quotes is what keeps the
        // document parseable while the readings differ. If a future edit
        // balanced them, this case would silently stop discriminating.
        EXPECT_EQ(std::count(doc.begin(), doc.end(), '"') % 2 +
                      std::count(doc.begin(), doc.end(), '\'') % 2,
                  1)
            << "case no longer has an odd quote count, so it no longer "
               "distinguishes the two readings: " << c;
    }

    // (b) The BALANCED-quote family, which is what the old premise sampled.
    //     pugixml really does refuse these, and PDFE answers fail-long. Kept
    //     so the record shows what was measured and why it misled.
    const char* refused[] = {
        "<!DOCTYPE html [ <![X[ \"]]>\" ]><r/>",
        "<!DOCTYPE html [ <![IGNORE[ \"]]>\" ]]> ]>",
    };
    for (const char* c : refused) {
        const std::string doc(c);
        EXPECT_EQ(pugiExtent(doc), std::string::npos)
            << "expected pugixml to refuse the balanced-quote form: " << c;
        EXPECT_EQ(doctypeExtent(doc, 0), doc.size())
            << "a document the parser refuses must get the longest extent";
    }

    // (c) THE CONSEQUENCE, which is why the direction matters. Quote tracking
    //     would push the extent to EOF on the first document above, so the
    //     element-content CDATA that follows would be scanned as DTD and this
    //     entirely legitimate document would be REFUSED -- a false positive,
    //     the failure mode XmlSafety.h warns about. It must be accepted.
    const std::string legitimate =
        "<!DOCTYPE html [ <![X[ \"]]>\" y\" ]>"
        "<html><body><![CDATA[<!ENTITY e SYSTEM \"http://h/\">]]></body></html>";
    const Verdict v = inspect(legitimate, kLenient);
    EXPECT_TRUE(v.safe())
        << "element content after a discriminating DOCTYPE must stay element "
           "content; refused as " << describe(v.reason)
        << " -- this is what a longer-than-pugixml extent costs";
    {
        pugi::xml_document d;
        EXPECT_TRUE(d.load_buffer(legitimate.data(), legitimate.size(),
                                  pugi::parse_default))
            << "the control must actually parse, or accepting it proves "
               "nothing";
    }

    // (d) ... and the guard still fires on the same shape when the declaration
    //     is genuinely inside the DTD rather than after it.
    EXPECT_EQ(inspect("<!DOCTYPE html [ <![X[ \"]]>\" y\" "
                      "<!ENTITY e SYSTEM \"h\"> ]><html/>",
                      kLenient).reason,
              Reject::ExternalIdentifier);
    EXPECT_EQ(inspect("<!DOCTYPE html [ <![INCLUDE[ <!ENTITY a \"]]>\"> ]]> "
                      "<!ENTITY e SYSTEM \"h\"> ]><html/>",
                      kLenient).reason,
              Reject::ExternalIdentifier);
}

// DIVERGENCE 2, kept with its reason. pugixml's group treats a bare `"` or `'`
// in DTD text as a primitive and skips to its match. `scan_internal_subset()`
// does NOT, because it is looking for something pugixml never looks for: a
// bare `%pe;` recall. A quoted literal between declarations is not valid DTD,
// and not skipping it means a `%pe;` hidden inside one is still flagged —
// stricter, and in the direction this function exists for.
// KILLS: "tidying" the subset walker to skip quoted literals for symmetry.
TEST(CoreXmlSafetyDoctypeStructure, SubsetTextQuotesAreNotSkippedAndWhy)
{
    // A `%pe;` inside a stray quoted literal in SUBSET TEXT is still flagged.
    EXPECT_EQ(inspect("<!DOCTYPE html [ \" %pe; \" ]><html/>", kLenient).reason,
              Reject::ParameterEntityRef);
    EXPECT_EQ(inspect("<!DOCTYPE html [ ' %pe; ' ]><html/>", kLenient).reason,
              Reject::ParameterEntityRef);

    // ... whereas inside a DECLARATION's literal it is NOT a reference, because
    // the declaration is stepped over as a unit. That distinction is the reason
    // the divergence is safe: it does not make every percent sign a finding.
    EXPECT_EQ(inspect("<!DOCTYPE html [ <!ATTLIST x y CDATA \"%notaref;\"> ]>"
                      "<html/>", kLenient).reason,
              Reject::DoctypeDeclaration);
    EXPECT_EQ(inspect("<!DOCTYPE html [ <!ATTLIST x y CDATA '%alsonot;'> ]>"
                      "<html/>", kLenient).reason,
              Reject::DoctypeDeclaration);

    // ... and ordinary documents with quotes and percent signs are untouched.
    EXPECT_TRUE(inspect("<a w='100%'>50% \"off\"</a>", kStrict).safe());
}

// ---------------------------------------------------------------------------
// The Qt entity-expansion cap
// ---------------------------------------------------------------------------

// Header: Qt's own default is 4096; the cap is "set low enough that a nested
// entity bomb trips it on the first level of nesting while leaving room for
// the built-in &amp; &lt; &gt; &quot; &apos;".
// KILLS: raising the constant back to Qt's default (or to any value that no
// longer trips on first nesting), and lowering it below the 5 built-ins.
TEST(CoreXmlSafetyLimits, EntityExpansionLimitLeavesRoomForBuiltinsOnly)
{
    EXPECT_GE(kEntityExpansionLimit, 5)
        << "must not be below the five XML built-in entities";
    EXPECT_LE(kEntityExpansionLimit, 64)
        << "must stay far below Qt's 4096 default, or a nested-entity bomb no "
           "longer trips on its first level of nesting";
}

}  // namespace
