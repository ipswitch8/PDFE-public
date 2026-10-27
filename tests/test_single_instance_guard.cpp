// R3-02: single-instance / same-file data-loss guard tests.
//
// Verifies pdfe::app::SingleInstanceGuard behaves as a primary/secondary
// discriminator built on QLocalServer:
//   * the first guard on a given (unique, test-isolated) name is PRIMARY;
//   * a second guard on the SAME name is SECONDARY (no second primary);
//   * a secondary forwarding a file path causes the primary to emit
//     fileOpenRequested(path) with the exact payload — i.e. a double-launch
//     on the same file is funneled into the one running editor, closing the
//     concurrent-save data-loss window instead of spawning a rival editor.
//
// Headless-friendly: QLocalServer/QLocalSocket need only a QCoreApplication,
// which the GoogleTest harness (test_main.cpp) provides. Each test uses a
// unique server name so runs never collide with a real PDFE or each other.

#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include <filesystem>
#include <memory>
#include <system_error>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSignalSpy>
#include <QString>
#include <QUuid>

#include "app/SingleInstanceGuard.h"

using pdfe::app::SingleInstanceGuard;

namespace {

// A unique-per-invocation server name so parallel runs / a live PDFE never
// interfere with the test's private single-instance domain.
QString uniqueName(const char* tag)
{
    return QStringLiteral("PDFE-test-%1-%2")
        .arg(tag)
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

// F63 — set/clear PDFE_INSTANCE_DOMAIN for the duration of a scope, restoring
// whatever was there before. The suite itself may be running WITH a domain set
// (ShellAndInstanceLaunchTest sets one per process), so a test that wants the
// unset case must actively unset it rather than assume it.
class ScopedInstanceDomain {
public:
    explicit ScopedInstanceDomain(const QString& value)
        : had_(qEnvironmentVariableIsSet("PDFE_INSTANCE_DOMAIN")),
          previous_(qEnvironmentVariable("PDFE_INSTANCE_DOMAIN"))
    {
        qputenv("PDFE_INSTANCE_DOMAIN", value.toUtf8());
    }
    ~ScopedInstanceDomain()
    {
        if (had_) {
            qputenv("PDFE_INSTANCE_DOMAIN", previous_.toUtf8());
        } else {
            qunsetenv("PDFE_INSTANCE_DOMAIN");
        }
    }
    ScopedInstanceDomain(const ScopedInstanceDomain&) = delete;
    ScopedInstanceDomain& operator=(const ScopedInstanceDomain&) = delete;

private:
    bool had_;
    QString previous_;
};

class ScopedUnsetInstanceDomain {
public:
    ScopedUnsetInstanceDomain()
        : had_(qEnvironmentVariableIsSet("PDFE_INSTANCE_DOMAIN")),
          previous_(qEnvironmentVariable("PDFE_INSTANCE_DOMAIN"))
    {
        qunsetenv("PDFE_INSTANCE_DOMAIN");
    }
    ~ScopedUnsetInstanceDomain()
    {
        if (had_) {
            qputenv("PDFE_INSTANCE_DOMAIN", previous_.toUtf8());
        }
    }
    ScopedUnsetInstanceDomain(const ScopedUnsetInstanceDomain&) = delete;
    ScopedUnsetInstanceDomain& operator=(const ScopedUnsetInstanceDomain&) = delete;

private:
    bool had_;
    QString previous_;
};

// Spin the event loop until `pred` is true or the deadline elapses. Returns
// pred()'s final value. Keeps the tests deterministic without fixed sleeps.
template <typename Pred>
bool spinUntil(Pred pred, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!pred() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return pred();
}

}  // namespace

// The first guard on a fresh name must own the instance.
TEST(SingleInstanceGuard, FirstInstanceIsPrimary)
{
    SingleInstanceGuard primary(uniqueName("primary"));
    EXPECT_TRUE(primary.isPrimary());
}

// A second guard on the same name must NOT become a second primary — exactly
// one editor owns the document domain.
TEST(SingleInstanceGuard, SecondInstanceIsSecondary)
{
    const QString name = uniqueName("dup");
    SingleInstanceGuard primary(name);
    ASSERT_TRUE(primary.isPrimary());

    SingleInstanceGuard secondary(name);
    EXPECT_FALSE(secondary.isPrimary());

    // Exactly one primary.
    EXPECT_TRUE(primary.isPrimary());
    EXPECT_FALSE(secondary.isPrimary());
}

// The core data-loss guarantee: a secondary launched on a file forwards the
// path to the running primary, which emits fileOpenRequested(path) — instead
// of a second editor opening the same file.
TEST(SingleInstanceGuard, SecondaryForwardsPathToPrimary)
{
    const QString name = uniqueName("forward");
    SingleInstanceGuard primary(name);
    ASSERT_TRUE(primary.isPrimary());

    QSignalSpy spy(&primary, &SingleInstanceGuard::fileOpenRequested);
    ASSERT_TRUE(spy.isValid());

    const QString wantPath = QStringLiteral("C:/tmp/shared-document.pdf");

    SingleInstanceGuard secondary(name);
    ASSERT_FALSE(secondary.isPrimary());
    ASSERT_TRUE(secondary.sendPathToPrimary(wantPath));

    ASSERT_TRUE(spinUntil([&] { return spy.count() >= 1; }));
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), wantPath);
}

// A bare forward with no path (double-click that is really "just focus the
// running editor") still reaches the primary as an empty request, so the
// window can be raised without opening anything.
TEST(SingleInstanceGuard, SecondaryForwardsEmptyPathAsActivate)
{
    const QString name = uniqueName("activate");
    SingleInstanceGuard primary(name);
    ASSERT_TRUE(primary.isPrimary());

    QSignalSpy spy(&primary, &SingleInstanceGuard::fileOpenRequested);
    ASSERT_TRUE(spy.isValid());

    SingleInstanceGuard secondary(name);
    ASSERT_FALSE(secondary.isPrimary());
    ASSERT_TRUE(secondary.sendPathToPrimary(QString()));

    ASSERT_TRUE(spinUntil([&] { return spy.count() >= 1; }));
    ASSERT_EQ(spy.count(), 1);
    EXPECT_TRUE(spy.at(0).at(0).toString().isEmpty());
}

// A path containing UTF-8 (non-ASCII) codepoints round-trips exactly through
// the local-socket wire protocol.
TEST(SingleInstanceGuard, ForwardsUtf8PathIntact)
{
    const QString name = uniqueName("utf8");
    SingleInstanceGuard primary(name);
    ASSERT_TRUE(primary.isPrimary());

    QSignalSpy spy(&primary, &SingleInstanceGuard::fileOpenRequested);
    ASSERT_TRUE(spy.isValid());

    // "C:/documents/résumé-日本語.pdf"
    const QString wantPath =
        QString::fromUtf8("C:/documents/r\xC3\xA9sum\xC3\xA9-"
                          "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.pdf");

    SingleInstanceGuard secondary(name);
    ASSERT_FALSE(secondary.isPrimary());
    ASSERT_TRUE(secondary.sendPathToPrimary(wantPath));

    ASSERT_TRUE(spinUntil([&] { return spy.count() >= 1; }));
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), wantPath);
}

// The production default server name is per-user (embeds the OS username) so
// separate logged-in users get isolated single-instance domains.
TEST(SingleInstanceGuard, DefaultNameIsPerUser)
{
    ScopedUnsetInstanceDomain noDomain;
    const QString def = SingleInstanceGuard::defaultServerName();
    EXPECT_TRUE(def.startsWith(QStringLiteral("PDFE-single-instance-")));
    EXPECT_GT(def.size(),
              QStringLiteral("PDFE-single-instance-").size());
}

// ── F63 — PDFE_INSTANCE_DOMAIN partitions the domain ────────────────────
//
// Per-user is the right granularity for a human and the wrong one for a test
// host, where several unrelated processes run concurrently as one account.
// Measured before any code changed: five concurrent pdfe_tests.exe processes
// collide and exactly the two ShellAndInstanceLaunchTest cases that rely on the
// DEFAULT single-instance behaviour fail; the same five run serially pass every
// time. These cases pin the partitioning property directly, so it is not
// evidenced only by a script that has to be remembered and re-run.

// The shipping default for every real user is UNSET, and unset must be
// byte-identical to the pre-F63 name. A change that made the suffix
// unconditional would silently give every user a new domain — and on the very
// first launch after an upgrade, an already-running editor would stop being
// found, which is the data-loss window this whole class exists to close.
TEST(SingleInstanceGuard, AnUnsetDomainLeavesTheNameExactlyAsItWas)
{
    QString withDomain;
    {
        ScopedInstanceDomain domain(QStringLiteral("shard-7"));
        withDomain = SingleInstanceGuard::defaultServerName();
    }
    ScopedUnsetInstanceDomain noDomain;
    const QString plain = SingleInstanceGuard::defaultServerName();

    EXPECT_NE(withDomain, plain);
    EXPECT_FALSE(plain.endsWith(QStringLiteral("-shard-7")));
    // Not merely "different": the plain name must be the exact prefix the
    // suffixed one is built from, which is what makes the suffix additive.
    EXPECT_EQ(withDomain, plain + QStringLiteral("-shard-7"));
}

// The property F63 actually needs: two processes given DIFFERENT domains must
// not share an instance domain. Without this, a concurrent sibling's primary
// answers a forward that was meant for one's own.
TEST(SingleInstanceGuard, DifferentDomainsDoNotShareAnInstanceDomain)
{
    QString a;
    QString b;
    {
        ScopedInstanceDomain domain(QStringLiteral("t1000-11111"));
        a = SingleInstanceGuard::defaultServerName();
    }
    {
        ScopedInstanceDomain domain(QStringLiteral("t2000-22222"));
        b = SingleInstanceGuard::defaultServerName();
    }
    EXPECT_NE(a, b);

    // And the isolation must be REAL, not just a different string: a guard on
    // each name must both be PRIMARY. A string comparison alone would still
    // pass if the names collided at the OS object level.
    SingleInstanceGuard first(a);
    SingleInstanceGuard second(b);
    EXPECT_TRUE(first.isPrimary());
    EXPECT_TRUE(second.isPrimary())
        << "two distinct domains must each get their own primary; if this "
           "fails the partition is cosmetic";
}

// The suffix must never REPLACE the per-user part. A domain that dropped the
// username would let two logged-in accounts that happen to pick the same domain
// string share one editor — widening the very isolation the per-user key exists
// to provide, in the name of narrowing it.
TEST(SingleInstanceGuard, ADomainNarrowsTheNameAndNeverWidensIt)
{
    ScopedUnsetInstanceDomain noDomain;
    const QString plain = SingleInstanceGuard::defaultServerName();

    ScopedInstanceDomain domain(QStringLiteral("anything"));
    const QString suffixed = SingleInstanceGuard::defaultServerName();

    EXPECT_TRUE(suffixed.startsWith(plain))
        << "the per-user part must survive; plain=\"" << plain.toStdString()
        << "\" suffixed=\"" << suffixed.toStdString() << "\"";
    EXPECT_GT(suffixed.size(), plain.size());
}

// ---------------------------------------------------------------------------
// GitHub #84 (WIN-54) -- a non-ASCII path forwarded through the guard must
// name a document that ACTUALLY OPENS.
//
// ForwardsUtf8PathIntact above proves the STRING survives the IPC hop. That is
// necessary and not sufficient: it compares the received QString against the
// sent QString, so a transport that mangled both ends identically would pass,
// and nothing establishes that the surviving string still denotes a real file
// the product can open.
//
// This closes that: a real fixture is placed at a directory AND filename
// carrying CJK, Cyrillic, Arabic and accented Latin, forwarded through the
// guard, and the RECEIVED path -- not the sent one -- is what gets opened,
// saved and reopened. Asserting against the received value is the point; using
// the local variable would test nothing about the hop.
//
// Written with \x escapes so the source stays ASCII, matching the test above.
// ---------------------------------------------------------------------------
TEST(SingleInstanceGuard, ForwardedNonAsciiPathOpensSavesAndReopens)
{
    namespace fs = std::filesystem;

    const fs::path fixture = fs::path(TEST_FIXTURES_DIR) / "test_single.pdf";
    ASSERT_TRUE(fs::exists(fixture)) << "missing fixture test_single.pdf";

    // Directory: "pdfe_i18n_日本語-Привет"   File: "résumé-العربية.pdf"
    const QString dirName =
        QString::fromUtf8("pdfe_i18n_\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E-"
                          "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
    const QString fileName =
        QString::fromUtf8("r\xC3\xA9sum\xC3\xA9-"
                          "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9.pdf");

    std::error_code ec;
    const fs::path dir =
        fs::temp_directory_path() / fs::path(dirName.toStdU16String());
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    ASSERT_TRUE(fs::exists(dir))
        << "could not create a non-ASCII directory: " << ec.message();

    const fs::path target = dir / fs::path(fileName.toStdU16String());
    fs::copy_file(fixture, target, fs::copy_options::overwrite_existing, ec);
    ASSERT_TRUE(fs::exists(target))
        << "could not place the fixture at a non-ASCII path: " << ec.message();

    // Forward it through the single-instance IPC hop.
    const QString name = uniqueName("utf8open");
    SingleInstanceGuard primary(name);
    ASSERT_TRUE(primary.isPrimary());

    QSignalSpy spy(&primary, &SingleInstanceGuard::fileOpenRequested);
    ASSERT_TRUE(spy.isValid());

    const QString sentPath = QString::fromStdU16String(target.u16string());
    {
        SingleInstanceGuard secondary(name);
        ASSERT_FALSE(secondary.isPrimary());
        ASSERT_TRUE(secondary.sendPathToPrimary(sentPath));
    }

    ASSERT_TRUE(spinUntil([&] { return spy.count() >= 1; }));
    const QString receivedPath = spy.at(0).at(0).toString();
    EXPECT_EQ(receivedPath, sentPath);

    // The half ForwardsUtf8PathIntact cannot reach: open what ARRIVED.
    const fs::path arrived(receivedPath.toStdU16String());
    ASSERT_TRUE(fs::exists(arrived))
        << "the path that survived the IPC hop does not name a file on disk";

    auto c = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(c, arrived);
    ASSERT_NE(doc, nullptr) << "the forwarded non-ASCII path would not open";
    const int pagesBefore = doc->pageCount();
    EXPECT_GT(pagesBefore, 0);

    const fs::path out = dir / fs::path(
        QString::fromUtf8("saved-\xC3\xA9\xC3\xBC.pdf").toStdU16String());
    fs::remove(out, ec);
    ASSERT_NO_THROW(doc->saveAs(out));
    ASSERT_TRUE(fs::exists(out)) << "saveAs to a non-ASCII path wrote nothing";

    auto reopened = pdfe::core::Document::open(c, out);
    ASSERT_NE(reopened, nullptr) << "the saved non-ASCII file will not reopen";
    EXPECT_EQ(reopened->pageCount(), pagesBefore);

    reopened.reset();
    doc.reset();
    fs::remove_all(dir, ec);
}
