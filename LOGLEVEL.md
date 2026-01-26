# ElevenLabs ConvAI Module - Loglevel Management

## Огляд

Модуль підтримує гнучке управління рівнем логування як через конфігураційний файл, так і через Asterisk CLI в runtime.

## Рівні логування

- **off** (0) - Вимкнено всі логи модуля
- **error** (1) - Тільки критичні помилки
- **warning** (2) - Помилки + попередження
- **notice** (3) - **За замовчуванням**. Помилки + попередження + важливі NOTICE повідомлення
- **debug** (4) - Всі повідомлення, включаючи детальну відладку

## Конфігурація

### В файлі `elevenlabs.conf`:

```ini
[general]
; Встановлення рівня логування
loglevel=notice

; АБО використання legacy опції (для зворотної сумісності):
debug=false   ; false = loglevel буде notice (якщо не встановлено явно)
debug=true    ; true = loglevel буде debug
```

### Приклади конфігурацій:

```ini
# Тиха робота (тільки помилки)
[general]
loglevel=error

# Стандартна робота
[general]
loglevel=notice

# Повна відладка
[general]
loglevel=debug
```

## CLI Команди

### Перегляд поточного рівня

```bash
*CLI> elevenlabs loglevel
ElevenLabs loglevel: notice
```

### Зміна рівня в runtime

```bash
# Вимкнути всі логи
*CLI> elevenlabs loglevel off

# Тільки помилки
*CLI> elevenlabs loglevel error

# Помилки + попередження
*CLI> elevenlabs loglevel warning

# Стандартний рівень (помилки + попередження + NOTICE)
*CLI> elevenlabs loglevel notice

# Повна відладка
*CLI> elevenlabs loglevel debug
```

### Перегляд налаштувань модуля

```bash
*CLI> elevenlabs show settings

=== ElevenLabs Module Settings ===
Log level: notice
Debug: off

Configured agents: 2
  [kvk] agent_id=abc123, debug=off
  [adelina_hr] agent_id=xyz789, debug=off
```

### Legacy команда (для зворотної сумісності)

```bash
*CLI> elevenlabs debug on   # Встановлює loglevel=debug
*CLI> elevenlabs debug off  # Встановлює loglevel=notice
```

## Використання в production

### Рекомендовані налаштування:

**Для тихої роботи** (мінімум логів):
```ini
[general]
loglevel=warning
```

**Для нормальної експлуатації**:
```ini
[general]
loglevel=notice
```

**Для відладки проблем**:
```ini
[general]
loglevel=debug
```

## Runtime зміна без перезавантаження

Всі зміни через CLI застосовуються **миттєво** без потреби в:
- Перезавантаженні модуля
- Перезавантаженні Asterisk
- Перečитуванні конфігурації

```bash
# Миттєво вимкнути всі NOTICE повідомлення
*CLI> elevenlabs loglevel warning
ElevenLabs loglevel set to: warning

# Повернути назад
*CLI> elevenlabs loglevel notice
ElevenLabs loglevel set to: notice
```

## Технічні деталі

- **117** LOG_NOTICE повідомлень тепер контролюються через loglevel
- **55** LOG_DEBUG повідомлень контролюються через loglevel
- LOG_ERROR і LOG_WARNING **завжди** виводяться (незалежно від loglevel)
- Loglevel зберігається тільки в пам'яті, при перезавантаженні Asterisk повертається значення з конфігу
