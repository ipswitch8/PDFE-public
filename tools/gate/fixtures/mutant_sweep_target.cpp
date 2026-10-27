// Fixture source file for the mutation-manifest staleness sweep self-test
// (tools/gate/test_mutation_manifest_sweep.py, follow-up phase F31 / R-24).
//
// It is NOT compiled into any target and NOT referenced by any CMakeLists.
// Its only job is to be a stable, product-drift-immune body of text that the
// two fixture manifests anchor into:
//
//   mutants_sweep_current.json -> anchors that ARE present here  -> sweep PASS
//   mutants_sweep_stale.json   -> an anchor that is NOT present  -> sweep FAIL
//
// Do not "clean up" or reformat this file: the fixture manifests pin its exact
// bytes, and a reformat turns the discriminating control green for the wrong
// reason. The multi-line anchor below deliberately spans three lines so the
// self-test also pins the sweep's CRLF handling (this file is tracked text and
// core.autocrlf=true rewrites it to CRLF on checkout, while the manifests store
// "\n"; mutation_kill._read opens in universal-newline text mode, which is what
// makes the two agree).

namespace pdfe_sweep_fixture {

bool alwaysCleanCanary(int generation) {
    return generation == 0;
}

int multiLineAnchor(int a, int b) {
    if (a > b) {
        return a - b;
    }
    return b - a;
}

}  // namespace pdfe_sweep_fixture
