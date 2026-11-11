# Badanaut — эмулятор Samsung bada

Badanaut запускает оригинальные приложения Samsung bada для ARM32 через HLE.
Новый интерфейс библиотеки одинаковый на Windows, Linux и Android и вдохновлён
[touchHLE / HyperHLE-Reborn](https://github.com/KlugKlugTG/HyperHLE-Reborn).

Игры представлены аккуратными значками со скруглением, подписями и глянцем.
Есть поиск, сортировка, страницы, нижняя панель, запуск последней игры и выбор
собственного значка. Движок эмулятора написан на C++17.

На Windows и Linux библиотека открывается в окне. На всех платформах игры по
умолчанию запускаются в полноэкранном режиме; его можно отключить в настройках.
На Windows и Linux F11 переключает полноэкранный режим игры. Настройки предлагают
масштабирование с сохранением пропорций, целочисленный масштаб, фильтрацию Canvas,
вертикальную синхронизацию и полноэкранный режим. Настройки применяются при следующем запуске;
внутреннее разрешение игры сохраняется.

Добавьте ZIP игры с `Bin/*.exe` и `Info/Appinfo.sys` или `Info/manifest.xml`,
затем выберите оригинальный ARM32 Bada `libstdc++.so.6` из SDK/прошивки с
совместимым float ABI. Игры и прошивка не входят в сборки. Установленные ранее
игры Android и данные сохраняются при обновлении.

В GitHub Actions доступны **Desktop builds** (Windows и Linux x86-64) и
**Android APK** (ARM64 и ARMv7). Скачайте артефакт успешной сборки и распакуйте его
целиком. Нативные зависимости закреплены как подмодули Git:

```sh
git clone --recurse-submodules https://github.com/j92580498-max/Badanaut.git
cd Badanaut
```

В существующей копии выполните `git submodule update --init --recursive`.

[Полная документация и сборка](README.md) · [Сборка Windows/Linux](frontends/pocket-desktop/README.md)

Движок основан на [mariotaku/bada-hle](https://github.com/mariotaku/bada-hle).
Атрибуция и условия: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
