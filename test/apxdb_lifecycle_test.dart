import 'dart:ffi';

import 'package:ffi/ffi.dart';
import 'package:test/test.dart';
import 'package:apxdb/src/apxdb_ffi.dart';

void main() {
  late bool nativeOpen;
  late ApxDbBindings bindings;

  setUp(() {
    nativeOpen = false;
    bindings = ApxDbBindings.test(
      open: (Pointer<Utf8> path) {
        nativeOpen = true;
        return ApxDbError.ok;
      },
      close: () {
        if (!nativeOpen) {
          return ApxDbError.notOpen;
        }
        nativeOpen = false;
        return ApxDbError.ok;
      },
      gpuStatus: () => ApxDbGpuStatus.vulkanActive,
      lastQueryPath: () => ApxDbQueryPath.cpuOnly,
      lastQueryDocCount: () => 0,
      lastQueryMetrics: (Pointer<ApxDbQueryMetricsNative> outMetrics) {
        final metrics = outMetrics.ref;
        metrics.bytesUploaded = 0;
        metrics.bytesReused = 0;
        metrics.cacheHits = 0;
        metrics.cacheMisses = 0;
        metrics.resultCount = 0;
        metrics.cpuPrepareNs = 0;
        metrics.gpuExecNs = 0;
        metrics.totalNs = 0;
        metrics.path = ApxDbQueryPath.cpuOnly;
        metrics.plan = ApxDbQueryPlan.scan;
        metrics.planReason = ApxDbQueryPlanReason.unspecified;
        metrics.selectivityEstimate = 0.0;
        metrics.numericDomainMin = 0.0;
        metrics.numericDomainMax = 0.0;
        metrics.rangeLower = 0.0;
        metrics.rangeUpper = 0.0;
        return 0;
      },
      createDocument: (Pointer<Utf8> json) => nullptr,
      createDocumentBytes: (Pointer<Uint8> bytes, int length) => nullptr,
      getDocumentBytes: (Pointer<Utf8> id, Pointer<IntPtr> outLength) =>
          nullptr,
      releaseBytes: (Pointer<Uint8> bytes) {},
      findDocument: (Pointer<Utf8> query) => nullptr,
      releaseString: (Pointer<Utf8> value) {},
    );
    ApxDB.setBindingsForTesting(bindings);
  });

  test('double open throws StateError', () {
    expect(ApxDB.open('test-dir'), equals(ApxDbError.ok));
    expect(() => ApxDB.open('test-dir'), throwsStateError);
  });

  test('gpu status is available after open', () {
    expect(ApxDB.open('test-dir'), equals(ApxDbError.ok));
    expect(ApxDB.gpuStatus(), equals(ApxDbGpuStatus.vulkanActive));
    expect(ApxDB.close(), equals(ApxDbError.ok));
  });

  test('query diagnostics bindings return default values', () {
    expect(ApxDB.lastQueryPath(), equals(ApxDbQueryPath.cpuOnly));
    expect(ApxDB.lastQueryDocCount(), equals(0));
  });

  test('last query metrics binding returns mapped diagnostics values', () {
    bindings = ApxDbBindings.test(
      open: (Pointer<Utf8> path) => ApxDbError.ok,
      close: () => ApxDbError.ok,
      gpuStatus: () => ApxDbGpuStatus.vulkanActive,
      lastQueryPath: () => ApxDbQueryPath.cpuOnly,
      lastQueryDocCount: () => 0,
      lastQueryMetrics: (Pointer<ApxDbQueryMetricsNative> outMetrics) {
        final metrics = outMetrics.ref;
        metrics.bytesUploaded = 1000;
        metrics.bytesReused = 50;
        metrics.cacheHits = 2;
        metrics.cacheMisses = 1;
        metrics.resultCount = 3;
        metrics.cpuPrepareNs = 10;
        metrics.gpuExecNs = 0;
        metrics.totalNs = 10;
        metrics.path = ApxDbQueryPath.cpuOnly;
        metrics.plan = ApxDbQueryPlan.indexRange;
        metrics.planReason = ApxDbQueryPlanReason.rangeNarrow;
        metrics.selectivityEstimate = 0.125;
        metrics.numericDomainMin = 0.0;
        metrics.numericDomainMax = 9.0;
        metrics.rangeLower = 8.0;
        metrics.rangeUpper = 9.0;
        return 0;
      },
      createDocument: (Pointer<Utf8> json) => nullptr,
      createDocumentBytes: (Pointer<Uint8> bytes, int length) => nullptr,
      getDocumentBytes: (Pointer<Utf8> id, Pointer<IntPtr> outLength) =>
          nullptr,
      releaseBytes: (Pointer<Uint8> bytes) {},
      findDocument: (Pointer<Utf8> query) => nullptr,
      releaseString: (Pointer<Utf8> value) {},
    );
    ApxDB.setBindingsForTesting(bindings);

    final metrics = ApxDB.lastQueryMetrics();
    expect(metrics.plan, equals(ApxDbQueryPlan.indexRange));
    expect(metrics.planReason, equals(ApxDbQueryPlanReason.rangeNarrow));
    expect(metrics.selectivityEstimate, equals(0.125));
    expect(metrics.numericDomainMin, equals(0.0));
    expect(metrics.numericDomainMax, equals(9.0));
    expect(metrics.rangeLower, equals(8.0));
    expect(metrics.rangeUpper, equals(9.0));
    expect(metrics.resultCount, equals(3));
  });

  test('diagnostics explain formats last query metrics', () {
    final metrics = ApxDbQueryMetrics(
      bytesUploaded: 1000,
      bytesReused: 50,
      cacheHits: 2,
      cacheMisses: 1,
      resultCount: 3,
      cpuPrepareNs: 10,
      gpuExecNs: 0,
      totalNs: 10,
      path: ApxDbQueryPath.cpuOnly,
      plan: ApxDbQueryPlan.indexRange,
      planReason: ApxDbQueryPlanReason.rangeNarrow,
      selectivityEstimate: 0.125,
      numericDomainMin: 0.0,
      numericDomainMax: 9.0,
      rangeLower: 8.0,
      rangeUpper: 9.0,
    );

    final explain = ApxDbQueryDiagnostics.explain(metrics);
    expect(explain, contains('plan: INDEX_RANGE'));
    expect(explain, contains('reason: RANGE_NARROW'));
    expect(explain, contains('selectivity: 0.1250'));
    expect(explain, contains('domain: [0.0, 9.0]'));
    expect(explain, contains('range: [8.0, 9.0]'));
  });

  test('double close returns notOpen after first close', () {
    expect(ApxDB.open('test-dir'), equals(ApxDbError.ok));
    expect(ApxDB.close(), equals(ApxDbError.ok));
    expect(ApxDB.close(), equals(ApxDbError.notOpen));
  });

  test('open fail then close returns notOpen', () {
    ApxDB.setBindingsForTesting(ApxDbBindings.test(
      open: (Pointer<Utf8> path) => ApxDbError.invalidArgument,
      close: () => ApxDbError.notOpen,
      gpuStatus: () => ApxDbGpuStatus.unavailable,
      lastQueryPath: () => ApxDbQueryPath.cpuOnly,
      lastQueryDocCount: () => 0,
      createDocument: (Pointer<Utf8> json) => nullptr,
      createDocumentBytes: (Pointer<Uint8> bytes, int length) => nullptr,
      getDocumentBytes: (Pointer<Utf8> id, Pointer<IntPtr> outLength) =>
          nullptr,
      lastQueryMetrics: (Pointer<ApxDbQueryMetricsNative> outMetrics) {
        final metrics = outMetrics.ref;
        metrics.bytesUploaded = 0;
        metrics.bytesReused = 0;
        metrics.cacheHits = 0;
        metrics.cacheMisses = 0;
        metrics.resultCount = 0;
        metrics.cpuPrepareNs = 0;
        metrics.gpuExecNs = 0;
        metrics.totalNs = 0;
        metrics.path = ApxDbQueryPath.cpuOnly;
        metrics.plan = ApxDbQueryPlan.scan;
        metrics.planReason = ApxDbQueryPlanReason.unspecified;
        metrics.selectivityEstimate = 0.0;
        metrics.numericDomainMin = 0.0;
        metrics.numericDomainMax = 0.0;
        metrics.rangeLower = 0.0;
        metrics.rangeUpper = 0.0;
        return 0;
      },
      releaseBytes: (Pointer<Uint8> bytes) {},
      findDocument: (Pointer<Utf8> query) => nullptr,
      releaseString: (Pointer<Utf8> value) {},
    ));

    expect(ApxDB.open('bad-path'), equals(ApxDbError.invalidArgument));
    expect(ApxDB.close(), equals(ApxDbError.notOpen));
  });

  test('open close open works across cycles', () {
    expect(ApxDB.open('test-dir'), equals(ApxDbError.ok));
    expect(ApxDB.close(), equals(ApxDbError.ok));
    expect(ApxDB.open('test-dir'), equals(ApxDbError.ok));
    expect(ApxDB.close(), equals(ApxDbError.ok));
  });

  test('rebind after close simulates hot reload attach', () {
    expect(ApxDB.open('test-dir'), equals(ApxDbError.ok));
    expect(ApxDB.close(), equals(ApxDbError.ok));

    final secondBindings = ApxDbBindings.test(
      open: (Pointer<Utf8> path) => ApxDbError.ok,
      close: () => ApxDbError.ok,
      gpuStatus: () => ApxDbGpuStatus.unavailable,
      lastQueryPath: () => ApxDbQueryPath.cpuOnly,
      lastQueryDocCount: () => 0,
      lastQueryMetrics: (Pointer<ApxDbQueryMetricsNative> outMetrics) {
        final metrics = outMetrics.ref;
        metrics.bytesUploaded = 0;
        metrics.bytesReused = 0;
        metrics.cacheHits = 0;
        metrics.cacheMisses = 0;
        metrics.resultCount = 0;
        metrics.cpuPrepareNs = 0;
        metrics.gpuExecNs = 0;
        metrics.totalNs = 0;
        metrics.path = ApxDbQueryPath.cpuOnly;
        metrics.plan = ApxDbQueryPlan.scan;
        metrics.planReason = ApxDbQueryPlanReason.unspecified;
        metrics.selectivityEstimate = 0.0;
        metrics.numericDomainMin = 0.0;
        metrics.numericDomainMax = 0.0;
        metrics.rangeLower = 0.0;
        metrics.rangeUpper = 0.0;
        return 0;
      },
      createDocument: (Pointer<Utf8> json) => nullptr,
      createDocumentBytes: (Pointer<Uint8> bytes, int length) => nullptr,
      getDocumentBytes: (Pointer<Utf8> id, Pointer<IntPtr> outLength) =>
          nullptr,
      releaseBytes: (Pointer<Uint8> bytes) {},
      findDocument: (Pointer<Utf8> query) => nullptr,
      releaseString: (Pointer<Utf8> value) {},
    );
    ApxDB.setBindingsForTesting(secondBindings);

    expect(ApxDB.open('test-dir'), equals(ApxDbError.ok));
    expect(ApxDB.close(), equals(ApxDbError.ok));
  });
}
