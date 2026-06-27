import 'dart:ffi';

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
  static int lastQueryPath() => _bindings.lastQueryPath();
  static int lastQueryDocCount() => _bindings.lastQueryDocCount();

  static String createDocument(String json) {
    final jsonPointer = json.toNativeUtf8();
    final resultPointer = _bindings.createDocument(jsonPointer);
    calloc.free(jsonPointer);
    final result = resultPointer.toDartString();
    _bindings.releaseString(resultPointer);
    return result;
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

class ApxDbQueryPath {
  static const int cpuOnly = 0;
  static const int gpuUsed = 1;
  static const int gpuSkippedThreshold = 2;
  static const int gpuSkippedUnsupportedType = 3;
  static const int gpuSkippedBackendUnavailable = 4;
}
