import 'dart:ffi';
import 'dart:io';

DynamicLibrary openApxDbLibrary() {
  if (Platform.isAndroid) {
    return DynamicLibrary.open('libapxdb.so');
  }
  if (Platform.isIOS || Platform.isMacOS) {
    return DynamicLibrary.process();
  }
  throw UnsupportedError(
      'ApxDB native library is not supported on this platform.');
}
