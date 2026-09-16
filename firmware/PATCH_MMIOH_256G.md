# Patch MMIOH 256 GiB — BTC79X5 (IvtQpiandMrcInit, Intel RefCode)

## Цель

4× CMP 50HX, BAR1 = 32 GiB/GPU → суммарно ~224 GiB MMIO → нужен MMIOH 256 GiB.

## Причина ограничения (подтверждено дизассемблированием)

Потолка «128 ГиБ» в коде НЕТ. Размер хранится как WORD `[esi+0x2C4]`,
читается из Setup[0xD9] 8-битным чтением, и масштабируется множителем
`0x40000000` (1 GiB). Единственный guard — «0 → 2 GiB» (строка WARNING).
Ни одного `cmp …, 0x80` / клампа по верхней границе нет. Ограничение 128 ГиБ
сидит только в 8-битном поле Setup (значения 0x01..0x80) и в меню IFR.

## Суть патча

Множитель 1 GiB → 2 GiB. Тогда Setup-значение 0x80 («128G» в меню) даёт
256 ГиБ. Патч — 1 байт, immediate не в .reloc (безопасно при любой базе).

## Точный байт

| | Адрес |
|---|---|
| Файл | `/ai/cmp/bios/rebar-xve-tu102-v2.rom` (8 МиБ) |
| ROM offset | `0x6CD3C9` |
| VMA | `0xFFECD3C9` (ImageBase 0xFFEC4EA0) |
| Было | `0x40` |
| Стало | `0x80` |

Инструкция (little-endian immediate):
```
Было:  B8 00 00 00 40   mov eax, 0x40000000   ; 1 GiB
Стало: B8 00 00 00 80   mov eax, 0x80000000   ; 2 GiB
```

ВНИМАНИЕ на смещение: меняется старший байт immediate, то есть **0x6CD3C9**
(байт `0x40`), а НЕ 0x6CD3C8 (там `0x00`). Патч по 0x6CD3C8 дал бы
0x40008000 — неверно.

## Дизассемблирование (функция масштабирования MMIOH)

VMA 0xFFECD386 … 0xFFECD3DA:

```
ffecd386: 0f b7 96 c4 02 00 00   movzx edx, WORD PTR [esi+0x2c4]  ; mmiohSize
ffecd38d: 66 85 d2                test   dx, dx                     ; == 0 ?
ffecd390: 75 2d                   jne    0xffecd3bf                 ; нет -> масштабирование
  ;; ветка 0 GB: печать WARNING, принудительно size = 2 (см. ниже)
ffecd392: 6a 0a                   push   0xa
ffecd394: b8 ff 00 00 00          mov    eax, 0xff
ffecd399: 50                      push   eax
ffecd39a: 50                      push   eax
ffecd39b: 50                      push   eax
ffecd39c: 56                      push   esi
ffecd39d: c7 45 08 02 00 00 00    mov    DWORD PTR [ebp+0x8], 0x2   ; force 2 GiB
ffecd3a4: e8 72 ef ff ff          call   0xffecc31b
ffecd3a9: 68 e0 4b f1 ff          push   0xfff14be0                  ; "WARNING: MMIOH size requested is 0GB per CPU..."
ffecd3ae: 68 00 00 00 02          push   0x2000000
ffecd3b3: 56                      push   esi
ffecd3b4: e8 52 e2 ff ff          call   0xffecb60b
ffecd3b9: 8b 55 08                mov    edx, DWORD PTR [ebp+0x8]   ; edx = 2 (forced)
ffecd3bc: 83 c4 20                add    esp, 0x20
ffecd3bf: 0f b7 d2                movzx  edx, dx                     ; edx = mmiohSize (16-bit)
ffecd3c2: 52                      push   edx                         ; arg: mmiohSize
ffecd3c3: 33 c9                   xor    ecx, ecx                    ; 0
ffecd3c5: b8 00 00 00 40          mov    eax, 0x40000000             ; <== 1 GiB множитель
ffecd3ca: 51                      push   ecx                         ; arg: 0
ffecd3cb: 50                      push   eax                         ; arg: 1 GiB
ffecd3cc: e8 c4 d6 03 00          call   0xfff0aa95                  ; вычисление (limit)
ffecd3d1: 6a 01                   push   0x1
ffecd3d3: 52                      push   edx
ffecd3d4: 50                      push   eax
ffecd3d5: e8 ab d7 03 00          call   0xfff0ab85                  ; запись LMMIOH
ffecd3da: 89 45 d4                mov    DWORD PTR [ebp-0x2c], eax
```

Строка-маркер `"mmiohSize: %u GB"` — по ROM 0x714EC1; `"mmiohSize: %08X"` —
по ROM 0x71E71A. Модуль IvtQpiandMrcInit: FFS GUID `5C08C7C8-24C2-4400-9627-CF2869421E06`,
ROM 0x6C4E48, размер 0x6AA7E (до 0x72F8C6). PE32 (MZ) открыт, несжатый,
ImageBase 0xFFEC4EA0.

## Проверка checksum (исправлено)

FFS header модуля (0x6C4E48): Attributes = 0x40 → FFS_ATTRIB_CHECKSUM УСТАНОВЛЕН.
IntegrityCheck = 0x99EC (Header=0xEC, File=0x99).

- Header checksum (байт 0xEC) — валиден: sum(header, File=0, State=0) = 0x00.
  Патч байта в теле файла header checksum НЕ меняет.
- Data checksum (байт 0x99) — валиден: по PI Spec считается по данным ПОСЛЕ
  24-байтного FFS header, checksum = (−sum(data)) & 0xFF. Проверка:
  0x67 + 0x99 = 0x00. Поэтому патч байта в теле ОБЯЗАН пересчитать data checksum.

После патча (0x6CD3C9: 0x40 → 0x80) data_sum 0x67 → 0xA7, новый File = 0x59.
Патч меняет ровно 2 байта: 0x6CD3C9 (множитель) + 0x6C4E59 (data checksum).

FV checksum не трогается: EFI_FIRMWARE_VOLUME_HEADER.Checksum покрывает только
заголовок FV, а не содержимое; изменение байта внутри PE32 его не ломает.

Итоговый образ (2 байта):
  rebar-xve-tu102-v2-mmioh256g.rom
  SHA256 4de9dc245e9bd26b497d8af95445e23fd15fbdf12bc540a2eb2df9a3d79e8165
Источник (неизменён):
  rebar-xve-tu102-v2.rom
  SHA256 34d0d4799379aae5320ef5ab84750bea9e2bfba6b52dae589b2f306918e69134

## Риски

1. Удваиваются ВСЕ значения Setup: 0x40 (64G) → 128 GiB, 0x80 (128G) → 256 GiB.
   Подписи меню IFR становятся косметически неверными. Нам нужен только 0x80.
2. Guard «0 → 2 GiB» с новым множителем даст 4 GiB — не используется (ставим 0x80).
3. Не проверено на железе. «Нет клампа» — вывод тщательного разбора, но
   окончательно подтверждается только загрузкой.
4. Прошивка — рискованная операция; нужен откат (бэкап текущего ROM) и
   способ восстановления (SPI-программатор / второй загрузочный носитель).

## Проверка после прошивки

```
dmesg | grep 'root bus resource'
# ждём верхний лимит 0x383FFFFFFFFF (было 0x381FFFFFFFFF при 128G)
lspci -vvv -s 01:00.0 | grep 'Region 1'   # 32 GiB
lspci -vvv -s 04:00.0 | grep 'Region 1'
nvidia-smi --query-gpu=memory.total --format=csv   # 20480 MiB
```
