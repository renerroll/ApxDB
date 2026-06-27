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
      createDocument: (Pointer<Utf8> json) => nullptr,
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
      createDocument: (Pointer<Utf8> json) => nullptr,
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
      createDocument: (Pointer<Utf8> json) => nullptr,
      findDocument: (Pointer<Utf8> query) => nullptr,
      releaseString: (Pointer<Utf8> value) {},
    );
    ApxDB.setBindingsForTesting(secondBindings);

    expect(ApxDB.open('test-dir'), equals(ApxDbError.ok));
    expect(ApxDB.close(), equals(ApxDbError.ok));
  });
}
