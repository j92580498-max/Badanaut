# Badanaut — эмулятор Samsung bada для Android

Badanaut запускает оригинальные 32-битные ARM-приложения Samsung bada через высокоуровневую эмуляцию. Android-интерфейс основан на лаунчере PocketHLE; проект также сохраняет эмуляцию Windows CE / Windows Mobile.

## Сборка APK

Понадобятся:

- JDK 17;
- Android SDK Platform 34 и Build Tools 34.0.0;
- Android NDK `26.1.10909125` и CMake `3.22.1`;
- стабильный Rust и `cargo-ndk` (`cargo install --locked cargo-ndk`).

Укажите каталог SDK в `ANDROID_SDK_ROOT` и запустите из корня репозитория:

```bash
./frontends/pocket-android/build-android.sh
```

Скрипт соберёт Unicorn-backed native library для `arm64-v8a` и `armeabi-v7a`, затем создаст `dist/PocketHLE-Bada-Android.apk`.

## Файлы игры и bada runtime

Архив игры и firmware-библиотека в репозитории намеренно отсутствуют. Чтобы включить ярлык Sky Force в APK, положите легально полученные файлы сюда:

```text
frontends/pocket-android/app/src/main/assets/bada/sky-force-spaces.im.zip
frontends/pocket-android/app/src/main/assets/bada/libstdc++.so.6
```

В ZIP должны лежать `Bin/SkyForce.exe`, `Info/Appinfo.sys` и `Res/data.pak` непосредственно в корне. Android-приложение распакует пакет в собственное закрытое хранилище. Не добавляйте игровые или firmware-файлы в Git.

## Структура исходников

- `frontends/pocket-android/` — интерфейс Android, SDL2 и сборка APK;
- `frontends/pocket-android/app/src/main/cpp/` — JNI/CMake-точка входа;
- `third_party/bada-hle/` — исходный код bada HLE;
- `crates/` — ядро PocketHLE и Windows CE API;
- `docs/BADA_ANDROID.md` — схема Android-сборки и запуска bada-приложений;
- `THIRD_PARTY_NOTICES.md` — происхождение компонентов и границы лицензий.

Исходник bada HLE опубликован в Badanaut с разрешения его автора, переданного сопровождающему репозитория. Это разрешение не меняет лицензию upstream-кода и не распространяется на другие проекты. Код PocketHLE остаётся под лицензией Apache-2.0 OR MIT; см. `THIRD_PARTY_NOTICES.md`.
