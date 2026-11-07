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

Скрипт соберёт Unicorn-backed native library для `arm64-v8a` и `armeabi-v7a`, затем создаст `dist/Badanaut-Android.apk`.

## Скачать APK из GitHub Actions

Откройте вкладку **Actions** в репозитории, выберите **Android APK** и успешный запуск. Внизу, в разделе **Artifacts**, скачайте **Badanaut-Android-…**, распакуйте ZIP и установите `Badanaut-Android.apk` на телефон.

CI собирает оба нативных движка для ARM64 и ARMv7, запускает Android-тесты, проверяет наличие библиотек и подпись APK, затем выкладывает сборку. Запуски происходят при изменениях в `main`, ветках `codex/**` и pull request. После добавления workflow в основную ветку новую сборку можно запустить кнопкой **Run workflow**. APK хранится в Artifacts 30 дней.

## Запуск Bada-игры на Android

1. Откройте раздел **Bada** — он выбран по умолчанию.
2. Нажмите **Добавить игру Bada** и выберите ZIP с пакетом игры. Внутри нужны `Bin/*.exe` (32-битный ARM ELF), `Info/Appinfo.sys` или `Info/manifest.xml` и ресурсы игры. Пакет может лежать во вложенной папке ZIP.
3. Нажмите **Выбрать библиотеку** и добавьте совместимую `libstdc++.so.6` или `libstdc++.so.6.0.12` из вашего Bada SDK/прошивки. Вариант hard-float или soft-float должен соответствовать игре.
4. Нажмите на карточку установленной игры для запуска. Меню карточки позволяет посмотреть сведения, заменить библиотеку среды и удалить игру.

Раздел **Pocket PC** сохраняет импорт CAB/ZIP/RAR/Windows EXE и запуск через PocketHLE. Пакеты Bada запускаются отдельным Bada-движком. Импорт пакета не гарантирует совместимость игры с текущей реализацией HLE.

## Необязательные встроенные файлы игры и bada runtime

Архивы игр и firmware-библиотека в репозитории намеренно отсутствуют. Их можно добавить на телефоне без пересборки APK. Чтобы заранее включить Sky Force в локальную сборку, положите легально полученные файлы сюда:

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
