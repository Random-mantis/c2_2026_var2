# Курсовая работа по системному программированию, вариант 2

Вариант 2: индекс для полей `INDEXED` реализован как B+-tree.

## Сборка

```powershell
cmake -S . -B build
cmake --build build
```

## Запуск

Интерактивный режим:

```powershell
.\build\Debug\cwdb.exe
```

Пакетный режим:

```powershell
.\build\Debug\cwdb.exe examples\variant2_demo.sql
```

Данные хранятся в каталоге `data`. Для каждого запроса ведется журнал `data/access.log`.
