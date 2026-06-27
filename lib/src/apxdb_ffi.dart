import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'native.dart';

typedef _CInitialize = Int32 Function();
typedef _DartInitialize = int Function();

typedef _CShutdown = Int32 Function();
typedef _DartShutdown = int Function();

typedef _CCreateDocument = Pointer<Utf8> Function(Pointer<Utf8> json);
typedef _DartCreateDocument = Pointer<Utf8> Function(Pointer<Utf8> json);

typedef _CFindDocument = Pointer<Utf8> Function(Pointer<Utf8> query);
typedef _DartFindDocument = Pointer<Utf8> Function(Pointer<Utf8> query);

typedef _CReleaseString = Void Function(Pointer<Utf8> value);
typedef _DartReleaseString = void Function(Pointer<Utf8> value);

class ApxDbBindings {
  final _DartInitialize initialize;
  final _DartShutdown shutdown;
  final _DartCreateDocument createDocument;
  final _DartFindDocument findDocument;
  final _DartReleaseString releaseString;

  ApxDbBindings(DynamicLibrary lib)
      : initialize = lib
            .lookupFunction<_CInitialize, _DartInitialize>('apxdb_initialize'),
        shutdown =
            lib.lookupFunction<_CShutdown, _DartShutdown>('apxdb_shutdown'),
        createDocument =
            lib.lookupFunction<_CCreateDocument, _DartCreateDocument>(
                'apxdb_create_document'),
        findDocument = lib.lookupFunction<_CFindDocument, _DartFindDocument>(
            'apxdb_find_document'),
        releaseString = lib.lookupFunction<_CReleaseString, _DartReleaseString>(
            'apxdb_release_string');
}

class ApxDB {
  static final ApxDbBindings _bindings = ApxDbBindings(openApxDbLibrary());

  static void initialize() => _bindings.initialize();

  static void shutdown() => _bindings.shutdown();

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
