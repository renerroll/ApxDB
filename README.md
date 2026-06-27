# ApxDB

`ApxDB` is a Flutter plugin skeleton for a compute-backed document NoSQL store.

It targets:
- Android via Kotlin + Vulkan/C++
- iOS via Objective-C/Metal/C++
- macOS via Objective-C/Metal/C++
- Flutter Dart FFI as the public interface

## Usage

```dart
import 'package:apxdb/apxdb.dart';

void main() {
  ApxDB.initialize();
  final id = ApxDB.createDocument('{"name":"Alice"}');
  final doc = ApxDB.findDocument(id);
  print('created id=$id document=$doc');
  ApxDB.shutdown();
}
```

## Build

- Android: the native C library is built by Gradle/CMake
- iOS/macOS: the plugin includes Objective-C++ wrappers that compile the shared native engine

## Schema and supported types

The native core now includes schema metadata support for document collections with primitive and complex field types.

Supported primitive types:
- `byte` (0–255)
- `short` (32-bit signed integer)
- `int` (64-bit signed integer)
- `float`
- `double`
- `bool` (`true`, `false`, `null`)
- `String` (UTF-8)
- `DateTime` (microsecond precision internally)

Also supported:
- embedded objects (`@embedded`-style nested schemas)
- list fields (`List<T>` variants)
- enums with serialization strategies: `ordinal`, `ordinal32`, `name`, `value`

Numeric null semantics use sentinel values instead of separate null markers, consistent with Isar's model.

> Current implementation focus: non-web platforms only (Android, iOS, macOS). Web support is intentionally deferred.

## Next steps

- Add real Vulkan compute kernel support in `src/native/apxdb.c`
- Add Metal compute kernel support in `ios/Classes/apxdb_native.mm` and `macos/Classes/apxdb_native.mm`
- Add an example Flutter app under `example/`
