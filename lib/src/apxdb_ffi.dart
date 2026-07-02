import 'dart:ffi';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'native.dart';

typedef _COpen = Int32 Function(Pointer<Utf8> path);
typedef _DartOpen = int Function(Pointer<Utf8> path);

typedef _CClose = Int32 Function();
typedef _DartClose = int Function();

typedef _CGpuStatus = Int32 Function();
typedef _DartGpuStatus = int Function();

typedef _CCreateDocument = Pointer<Utf8> Function(Pointer<Utf8> json);
typedef _DartCreateDocument = Pointer<Utf8> Function(Pointer<Utf8> json);

typedef _CCreateDocumentBytes = Pointer<Utf8> Function(
    Pointer<Uint8> bytes, IntPtr length);
typedef _DartCreateDocumentBytes = Pointer<Utf8> Function(
    Pointer<Uint8> bytes, int length);

typedef _CGetDocumentBytes = Pointer<Uint8> Function(
    Pointer<Utf8> id, Pointer<IntPtr> outLength);
typedef _DartGetDocumentBytes = Pointer<Uint8> Function(
    Pointer<Utf8> id, Pointer<IntPtr> outLength);

typedef _CReleaseBytes = Void Function(Pointer<Uint8> bytes);
typedef _DartReleaseBytes = void Function(Pointer<Uint8> bytes);

typedef _CFindDocument = Pointer<Utf8> Function(Pointer<Utf8> query);
typedef _DartFindDocument = Pointer<Utf8> Function(Pointer<Utf8> query);

typedef _CQueryPath = Int32 Function();
typedef _DartQueryPath = int Function();

typedef _CQueryDocCount = Uint32 Function();
typedef _DartQueryDocCount = int Function();

typedef _CReleaseString = Void Function(Pointer<Utf8> value);
typedef _DartReleaseString = void Function(Pointer<Utf8> value);

class ApxDbBindings {
  final _DartOpen open;
  final _DartClose close;
  final _DartGpuStatus gpuStatus;
  final _DartQueryPath lastQueryPath;
  final _DartQueryDocCount lastQueryDocCount;
  final _DartCreateDocument createDocument;
  final _DartCreateDocumentBytes createDocumentBytes;
  final _DartGetDocumentBytes getDocumentBytes;
  final _DartReleaseBytes releaseBytes;
  final _DartFindDocument findDocument;
  final _DartReleaseString releaseString;

  ApxDbBindings(DynamicLibrary lib)
      : open = lib.lookupFunction<_COpen, _DartOpen>('apxdb_open'),
        close = lib.lookupFunction<_CClose, _DartClose>('apxdb_close'),
        gpuStatus =
            lib.lookupFunction<_CGpuStatus, _DartGpuStatus>('apxdb_gpu_status'),
        lastQueryPath = lib.lookupFunction<_CQueryPath, _DartQueryPath>(
            'apxdb_last_query_path'),
        lastQueryDocCount =
            lib.lookupFunction<_CQueryDocCount, _DartQueryDocCount>(
                'apxdb_last_query_doc_count'),
        createDocument =
            lib.lookupFunction<_CCreateDocument, _DartCreateDocument>(
                'apxdb_create_document'),
        createDocumentBytes =
            lib.lookupFunction<_CCreateDocumentBytes, _DartCreateDocumentBytes>(
                'apxdb_create_document_bytes'),
        getDocumentBytes =
            lib.lookupFunction<_CGetDocumentBytes, _DartGetDocumentBytes>(
                'apxdb_get_document_bytes'),
        releaseBytes = lib.lookupFunction<_CReleaseBytes, _DartReleaseBytes>(
            'apxdb_release_bytes'),
        findDocument = lib.lookupFunction<_CFindDocument, _DartFindDocument>(
            'apxdb_find_document'),
        releaseString = lib.lookupFunction<_CReleaseString, _DartReleaseString>(
            'apxdb_release_string');

  ApxDbBindings.test({
    required this.open,
    required this.close,
    required this.gpuStatus,
    required this.lastQueryPath,
    required this.lastQueryDocCount,
    required this.createDocument,
    required this.createDocumentBytes,
    required this.getDocumentBytes,
    required this.releaseBytes,
    required this.findDocument,
    required this.releaseString,
  });
}

class ApxDB {
  static ApxDbBindings _bindings = ApxDbBindings(openApxDbLibrary());
  static bool _isOpen = false;

  static void setBindingsForTesting(ApxDbBindings bindings) {
    _bindings = bindings;
    _isOpen = false;
  }

  static bool get isOpen => _isOpen;

  static int open(String directoryPath) {
    if (_isOpen) {
      throw StateError('ApxDB is already open. Close before re-opening.');
    }
    final pathPointer = directoryPath.toNativeUtf8();
    final result = _bindings.open(pathPointer);
    calloc.free(pathPointer);
    if (result == ApxDbError.ok || result == ApxDbError.okGpuFallback) {
      _isOpen = true;
    }
    return result;
  }

  static int close() {
    if (!_isOpen) {
      return ApxDbError.notOpen;
    }
    final result = _bindings.close();
    if (result == ApxDbError.ok) {
      _isOpen = false;
    }
    return result;
  }

  static int gpuStatus() => _bindings.gpuStatus();

  /// Debug-only diagnostics helper.
  ///
  /// Returns global runtime state for the last observed native query.
  /// This is not guaranteed to be correct for concurrent or isolate-shared
  /// execution and should be used only for debug/benchmarking.
  static int lastQueryPath() => _bindings.lastQueryPath();

  /// Debug-only diagnostics helper.
  ///
  /// Returns the document count observed for the last native query.
  /// This is not guaranteed to be correct for concurrent or isolate-shared
  /// execution and should be used only for debug/benchmarking.
  static int lastQueryDocCount() => _bindings.lastQueryDocCount();

  static String createDocument(String json) {
    final jsonPointer = json.toNativeUtf8();
    final resultPointer = _bindings.createDocument(jsonPointer);
    calloc.free(jsonPointer);
    final result = resultPointer.toDartString();
    _bindings.releaseString(resultPointer);
    return result;
  }

  static String createDocumentBytes(Uint8List bytes) {
    final buffer = calloc<Uint8>(bytes.length);
    final nativeBytes = buffer.asTypedList(bytes.length);
    nativeBytes.setAll(0, bytes);
    final resultPointer = _bindings.createDocumentBytes(buffer, bytes.length);
    calloc.free(buffer);
    final result = resultPointer.toDartString();
    _bindings.releaseString(resultPointer);
    return result;
  }

  static Uint8List getDocumentBytes(String id) {
    final idPointer = id.toNativeUtf8();
    final outLength = calloc<IntPtr>();
    final resultPointer = _bindings.getDocumentBytes(idPointer, outLength);
    calloc.free(idPointer);
    final length = outLength.value;
    calloc.free(outLength);
    if (resultPointer.address == 0 || length <= 0) {
      return Uint8List(0);
    }
    final bytes = resultPointer.asTypedList(length);
    final copy = Uint8List.fromList(bytes);
    _bindings.releaseBytes(resultPointer);
    return copy;
  }

  static String findDocument(String query) {
    final queryPointer = query.toNativeUtf8();
    final resultPointer = _bindings.findDocument(queryPointer);
    calloc.free(queryPointer);
    final result = resultPointer.toDartString();
    _bindings.releaseString(resultPointer);
    return result;
  }
}

class ApxDbError {
  static const int ok = 0;
  static const int okGpuFallback = 10;
  static const int alreadyOpen = 1;
  static const int notOpen = 2;
  static const int gpuInitFailed = 3;
  static const int partialRecovery = 4;
  static const int invalidArgument = 5;
  static const int io = 6;
  static const int unknown = 255;
}

class ApxDbGpuStatus {
  static const int unavailable = 0;
  static const int metalActive = 1;
  static const int vulkanActive = 2;
  static const int disabledByThreshold = 3;
  static const int initFailed = 4;
}

/// Debug-only query diagnostics values returned by [ApxDB.lastQueryPath].
/// These are a convenience API and are not thread-safe in the current
/// runtime implementation.
class ApxDbQueryPath {
  static const int cpuOnly = 0;
  static const int gpuUsed = 1;
  static const int gpuSkippedThreshold = 2;
  static const int gpuSkippedUnsupportedType = 3;
  static const int gpuSkippedBackendUnavailable = 4;
}
