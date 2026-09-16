# CMP 50HX (TU102) — полный стек: ReBAR 32 GiB + P2P + Gen2, с нуля на чистой Ubuntu 24.04

Проверено на живом железе 2026-09-16. Всё ниже — воспроизводимая процедура от голой
платы до рабочего GPU-хоста с BAR1 P2P. Команды — под Ubuntu 24.04.4 LTS (kernel 6.8),
плата BTC79X5 / X79 (Intel H61, LGA2011 Xeon E5), 2× NVIDIA CMP 50HX (TU102, `10de:1e09`,
20 GiB VRAM каждая).

Что получаем в итоге (эталонные цифры с рабочего хоста ai100gb):

| Параметр | Значение |
|---|---|
| BAR1 на карту | 32 GiB (Region 1 `[size=32G]`, адреса `0x382…`/`0x383…`) |
| MMIOH-пул | 256 GiB |
| PCIe линк | Gen2 x8 (5 GT/s) |
| VRAM | 20480 MiB на карту (не режется) |
| P2P capability | `OK` в обе стороны (`nvidia-smi topo -p2p r`) |
| Прямой P2P DMA | **2.47 GB/s** в обе стороны (≈90% от H2D-эталона 2.75 GB/s) |
| Целостность | 0 mismatch на peer-copy, host RAM не портится |
| Драйвер | 610.43.03 (open, форк aikitoria `610.43.03-p2p`), srcversion `8096CD86…` |

---

## 0. Требования по железу и доступ

- Плата BTC79X5 / X79ETH03 (Intel H61, LGA2011), BIOS AMI Aptio 4.6.5.
- 2–4× CMP 50HX (`10de:1e09`). У карт **нет видеовыхода** — для POST-диагностики и
  recovery обязательна отдельная видеокарта (напр. GT 710) + монитор.
- Прошивальщик SPI (CH341A + SOIC8) на случай кирпича.
- Физический доступ к тумблеру БП и джамперу CLR_CMOS.
- SSH + passwordless sudo (`sudo -n` работает) — для удалённых операций.

**Правило №1:** любая смена прошивки/селектора BAR требует физического доступа —
откат от зависшего POST удалённо не делается.

---

## 1. Прошивка BIOS (firmware-first ReBAR + 256 GiB MMIOH)

ReBAR на этой плате делается **только прошивкой** (DXE pre-pass, который пишет XVE TU102
до того, как `PciBusDxe` разберёт BAR). Любая попытка сделать то же из ядра/драйвера
(OS-side resize) стабильно валит RM: `kbusVerifyBar2_GM107 … garbage 0x0 →
RmInitAdapter failed (0x24:0x72)`. Не повторять.

Используем готовый образ `firmware/rebar-xve-tu102-v2-mmioh256g.rom` (8 MiB):
XVE pre-pass (16 GiB BAR1 base) + однобайтовый патч MMIOH 64→256 GiB.

### 1.1 Бэкап текущей прошивки (обязательно)

```bash
sudo flashrom -p internal -c MX25L6405 -r backup-current.rom   # 8 MiB
sha256sum backup-current.rom
```

Патчить надо **живой дамп**, а не чистый исходник — в живом дампе хранится NVRAM
(Setup-переменные, MRC memory-training, MAC).

### 1.2 Прошить

```bash
sudo flashrom -p internal -c MX25L6405 -w rebar-xve-tu102-v2-mmioh256g.rom
sudo flashrom -p internal -c MX25L6405 -v rebar-xve-tu102-v2-mmioh256g.rom
```

Судят по финальному `Verifying flash… VERIFIED`, а не по промежуточной ошибке
`ERASE FAILED` (flashrom подбирает другой erase-метод).

> Если хотите собрать образ сами из базового `rebar-xve-tu102-v2.rom` — см.
> `firmware/PATCH_MMIOH_256G.md` (патч `mov eax,0x40000000` → `0x80000000`, ROM `0x6CD3C9`,
> байт `0x40`→`0x80`, + пересчёт FFS data-checksum, скрипт `firmware/apply_patch.py`).
> Готовый `-mmioh256g.rom` уже содержит этот патч.

---

## 2. Селектор BAR1 = 32 GiB (efivar)

`XveBar1Selector` — одна глобальная EFI-переменная (все карты получают один размер BAR1):
`8` = 16 GiB, `9` = 32 GiB.

```bash
V=/sys/firmware/efi/efivars/XveBar1Selector-a3c5b77a-c88f-4a93-bf1c-4a92a32c65ce
sudo chattr -i "$V"                                  # efivarfs вешает immutable
printf '\x07\x00\x00\x00\x09' | sudo tee "$V" > /dev/null
sudo cat "$V" | od -An -tu1                          # ожидаем: 7 0 0 0 9
sudo chattr +i "$V"
```

**Почему 32 GiB не вешает POST (в отличие от старых попыток):** старый «selector 9 = POST
hang» был из-за MMIOH-пула по умолчанию 64 GiB — две карты по 32 GiB требуют ~96 GiB и не
влезали. Патч MMIOH 256 GiB (шаг 1) это снимает. Каждая 32 GiB-карта стоит 64 GiB MMIO
(32 GiB BAR1 + 32 GiB gap выравнивания моста): 2 карты = 96 GiB, 4 карты = 224 GiB — всё
влезает в 256 GiB. **Планируйте селектор под итоговое число карт ДО установки новых** —
добавление 3-й карты при живом селекторе 9 и 128 GiB-пуле снова вешает POST.

---

## 3. Чистая Ubuntu 24.04 + параметры ядра

Установить Ubuntu 24.04 LTS (server), затем в `/etc/default/grub`:

```
GRUB_CMDLINE_LINUX_DEFAULT="pci=realloc=on intel_iommu=on iommu=pt modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset,nvidia_uvm"
```

```bash
sudo update-grub
```

- `pci=realloc=on` — чтобы ядро переразместило крупные BAR.
- `intel_iommu=on iommu=pt` — IOMMU passthrough; **обязательно** для BAR1 P2P
  (translated-режим роняет peer-транзакции).
- `modprobe.blacklist=nvidia,…` — драйвер НЕ должен грузиться из early boot; его загрузит
  сервис после rescan (иначе ломается цепочка разблокировки). Важно именно в cmdline:
  `blacklist` в `/etc/modprobe.d` не держит (udev/boot-сервисы всё равно грузят модуль).

---

## 4. Драйвер NVIDIA 610.43.03 (форк с P2P)

### 4.1 Userspace (libcuda, GSP-прошивка, nvidia-smi) — стоковый

Установить официальный 610.43.03 **без ядровых модулей** (их соберём из форка):

```bash
# NVIDIA-Linux-x86_64-610.43.03.run
sudo ./NVIDIA-Linux-x86_64-610.43.03.run --no-kernel-modules
```

Это даёт `/usr/lib/x86_64-linux-gnu/libcuda.so.610.43.03`, GSP-firmware и `nvidia-smi`.
CUDA toolkit — отдельно (здесь 12.8, не обязателен для самого P2P).

### 4.2 Ядровые модули — форк aikitoria (P2P включён)

```bash
git clone -b 610.43.03-p2p https://github.com/aikitoria/open-gpu-kernel-modules.git
cd open-gpu-kernel-modules
./install.sh     # = rmmod → make modules -j$(nproc) → make modules_install → depmod → nvidia-smi
```

Что форк уже несёт (проверено чтением исходников — **доп. патчи НЕ нужны**):

- HAL-роутинг дефолтного (pre-Hopper) пути на GH100-реализации BAR1-P2P
  (`g_kern_bus_nvoc.c`; реализации chip-independent).
- `pcieP2PType=BAR1` по умолчанию и `p2pOverride=0x11` (READ+WRITE enable) в `kernel_bif.c`.
- Патчи bayley/cmpunlocker **0013** (skip mailbox peer pre-reg) и **0015** (force read-cap)
  — избыточны: 0013 сворачивается в мёртвый код (`gpumgrGetGpuLinkCount ≡ 0` ⇒
  `peerNumberMask` уже пуст), 0015 перекрывается `p2pOverride=0x11` (override выполняется
  раньше read-cap-функции).

### 4.3 Конфиг modprobe

`/etc/modprobe.d/cmp50hx-unlock.conf`:

```
options nvidia NVreg_EnablePCIeGen3=1 cmp50_rebar_size=0 NVreg_RegistryDwords="RMForceStaticBar1=1"
```

`/etc/modprobe.d/cmp-unlock.conf`:

```
blacklist nouveau
```

Пояснения:

- `NVreg_EnablePCIeGen3=1` — политика Gen3 (карта аппаратно капнута на Gen2 — `LnkCap`
  5 GT/s; параметр не вредит, Gen2 ставится ретрейном ниже).
- `cmp50_rebar_size=0` — ОБЯЗАТЕЛЬНО: отключает OS-side ReBAR-путь форка (иначе BAR2
  self-test падает). BAR1 делает прошивка, не драйвер.
- `NVreg_RegistryDwords="RMForceStaticBar1=1"` — заставляет драйвер использовать
  статическую BAR1. Это **отдельный** registry-DWORD, `pcieP2PType=BAR1` его не ставит;
  без него драйвер держит маленькую дефолтную BAR1, `kbusIsPcieBar1P2PMappingSupported`
  падает, и драйвер деградирует в mailbox-fallback (см. «безопасность»).

`RMPcieP2PType=1` **не нужен** — форк уже дефолтит `pcieP2PType=BAR1`.

После правки modprobe — `sudo update-initramfs -u`.

Проверка, что registry-DWORD реально применился (читать `/proc/driver/nvidia/params`, а не
`/sys/module/nvidia/parameters/` — на форке sysfs прячет NVreg_*):

```bash
cat /proc/driver/nvidia/params | grep -E 'EnablePCIeGen3|RegistryDwords'
# хотим: EnablePCIeGen3: 1  и  RegistryDwords: "RMForceStaticBar1=1"
```

---

## 5. Сервисы Gen2 (rescan + deferred retrain)

Две systemd-единицы, обе `enabled`. Они делают разное:

1. `cmp50hx-gen2-rescan.service` — pre-driver rescan: убирает CMP с шины, делает rescan
   (порядок разблокировки), грузит `nvidia` ровно один раз.
2. `cmp50hx-gen2.service` — deferred retrain: ждёт, пока карта сама разблокирует регистры
   скорости (TLS=2, наступает через секунды после GSP), и один раз дёргает retrain через
   upstream-порт. **Именно он реально поднимает Gen2** — pre-driver retrain не держится.

### 5.1 Скрипт pre-driver rescan — `/usr/local/sbin/cmp50hx-gen2-rescan.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail
CMP_VENDOR="0x10de"; CMP_DEVICE="0x1e09"
log() { echo "cmp50hx-gen2-rescan: $*"; }
get_gpus() {
  for dev in /sys/bus/pci/devices/*; do
    [[ -f "$dev/vendor" && -f "$dev/device" ]] || continue
    [[ "$(<"$dev/vendor")" == "$CMP_VENDOR" && "$(<"$dev/device")" == "$CMP_DEVICE" ]] && basename "$dev"
  done | sort
}
mapfile -t GPUS < <(get_gpus)
[[ ${#GPUS[@]} -gt 0 ]] || { log "no CMP 50HX"; exit 1; }
log "pre-driver rescan for ${#GPUS[@]} CMP: ${GPUS[*]}"
for gpu in "${GPUS[@]}"; do echo 1 > "/sys/bus/pci/devices/$gpu/remove"; done
echo 1 > /sys/bus/pci/rescan
sleep 3
for gpu in "${GPUS[@]}"; do
  upstream=$(basename "$(dirname "$(readlink -f "/sys/bus/pci/devices/$gpu")")")
  setpci -s "$gpu" CAP_EXP+30.W=0002:000f
  setpci -s "$upstream" CAP_EXP+30.W=0002:000f
  ctl=$(setpci -s "$upstream" CAP_EXP+10.W)
  setpci -s "$upstream" CAP_EXP+10.W="$(printf '%04x' $(( 0x${ctl} | 0x20 )))"
  for _ in $(seq 1 50); do
    speed=$(cat "/sys/bus/pci/devices/$gpu/current_link_speed" 2>/dev/null || :)
    [[ "$speed" == *"5.0 GT/s"* ]] && break
    sleep 0.1
  done
  log "$gpu: ${speed:-unknown} x$(cat "/sys/bus/pci/devices/$gpu/current_link_width")"
done
modprobe nvidia
modprobe nvidia_uvm
modprobe nvidia_modeset
modprobe nvidia_drm
log "driver loaded after rescan"
```

### 5.2 Скрипт deferred retrain — `/usr/local/sbin/cmp50hx-gen2`

```bash
#!/usr/bin/env bash
# CMP 50HX PCIe Gen2 auto-retrain. Патченый драйвер дёргает ретрейн при первом open (~8 с),
# но карта разблокирует регистры скорости на минуты позже — ранняя попытка не срабатывает и
# линк висит Gen1 навсегда. Этот сервис ждёт разблокировку (TLS=2) и ретрейнит один раз.
set -u
TIMEOUT_MIN=20; POLL_SEC=15; RETRAIN_WAIT_SEC=3
log() { echo "cmp50hx-gen2: $*"; }
link_gt_gen1() {
    case "$(cat "/sys/bus/pci/devices/$1/current_link_speed" 2>/dev/null)" in
        *5.0*|*8.0*|*16.0*|*32.0*) return 0 ;; *) return 1 ;;
    esac
}
retrain() {
    local ctl2 ctl
    ctl2=$(setpci -s "$1" CAP_EXP+30.W)
    setpci -s "$1" CAP_EXP+30.W="$(printf '%04x' $(( (0x$ctl2 & ~0xf) | 2 )))"
    ctl=$(setpci -s "$1" CAP_EXP+10.W)
    setpci -s "$1" CAP_EXP+10.W="$(printf '%04x' $(( 0x$ctl | 0x20 )))"
}
bfds=$(lspci -Dnn 2>/dev/null | awk '/10de:1e09/ {print $1}')
if [[ -z "${bfds}" ]]; then log "no CMP 50HX (10de:1e09) found; nothing to do"; exit 0; fi
deadline=$(( $(date +%s) + TIMEOUT_MIN * 60 ))
while :; do
    all_fast=1
    for bdf in ${bfds}; do
        if link_gt_gen1 "${bdf}"; then continue; fi
        all_fast=0
        tls=$(setpci -s "${bdf}" CAP_EXP+30.W 2>/dev/null)
        if [[ $(( 0x${tls:-0} & 0xf )) -eq 2 ]]; then
            upstream=$(basename "$(dirname "$(readlink -f "/sys/bus/pci/devices/${bdf}")")")
            log "${bdf}: card unlocked (TLS=2), firing retrain via ${upstream}"
            retrain "${upstream}"
            for _ in $(seq 1 $(( RETRAIN_WAIT_SEC * 10 ))); do
                link_gt_gen1 "${bdf}" && break; sleep 0.1
            done
            link_gt_gen1 "${bdf}" && log "${bdf}: PASS $(cat "/sys/bus/pci/devices/${bdf}/current_link_speed")" \
                || log "${bdf}: retrain fired, still Gen1 (will retry)"
        fi
    done
    [[ ${all_fast} -eq 1 ]] && { log "all CMP 50HX at Gen2 or better"; exit 0; }
    [[ $(date +%s) -ge ${deadline} ]] && { log "TIMEOUT after ${TIMEOUT_MIN} min"; exit 1; }
    sleep "${POLL_SEC}"
done
```

### 5.3 Юниты

`/etc/systemd/system/cmp50hx-gen2-rescan.service`:

```ini
[Unit]
Description=CMP 50HX PCIe Gen2 enable (pre-driver rescan + retrain)
After=sysinit.target
Before=multi-user.target llama-qwen.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/cmp50hx-gen2-rescan.sh
TimeoutStartSec=90
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

`/etc/systemd/system/cmp50hx-gen2.service`:

```ini
[Unit]
Description=CMP 50HX PCIe Gen2 auto-retrain (waits for the card to self-unlock)
Documentation=https://github.com/xrip/cmp50hx-unlock

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/cmp50hx-gen2
# the oneshot deliberately loops for minutes; give it room beyond TIMEOUT_MIN
TimeoutStartSec=25min

[Install]
WantedBy=multi-user.target
```

```bash
sudo chmod +x /usr/local/sbin/cmp50hx-gen2-rescan.sh /usr/local/sbin/cmp50hx-gen2
sudo systemctl daemon-reload
sudo systemctl enable --now cmp50hx-gen2-rescan.service cmp50hx-gen2.service
```

---

## 6. Reboot и проверка базового состояния

```bash
sudo systemctl reboot
```

После загрузки проверить **по порядку**:

```bash
lspci -nn -d 10de:                          # обе CMP перечислены
lspci -vvv -s 01:00.0 | grep -E 'Region 1'  # Region 1 [size=32G]
lspci -vvv -s 04:00.0 | grep -E 'Region 1'  # Region 1 [size=32G]
nvidia-smi --query-gpu=index,name,driver_version,memory.total,pcie.link.gen.current,pcie.link.width.current --format=csv
#   хотим: memory.total=20480 MiB, pcie.link.gen.current=2, width=8, на каждой карте
nvidia-smi topo -p2p r                      # P2P capability OK в обе стороны
systemctl is-active cmp50hx-gen2-rescan.service cmp50hx-gen2.service
journalctl -b -k | grep -E 'CMP50_GSP_READY|RmInitAdapter|garbage'   # GSP_READY, 0 ошибок
```

Важно: `pcie.link.gen.current` проверять **после** подъёма сервисов (см. `journalctl -u
cmp50hx-gen2`), а не по строке лога rescan — pre-driver retrain не держится, живой Gen2
ставит только deferred-юнит.

---

## 7. Проверка P2P (приёмочный тест)

«P2P работает» — это НЕ `nvidia-smi topo -p2p r` = OK. Приёмочный критерий: `OK` **и**
реальный bidirectional-тест целостности, где peer-копия быстрее staged.

Компилируемый runtime-API бенчмарк лежит в `scripts/p2p_benchmark.c` (gcc + libcudart):

```bash
gcc -O2 -I/usr/local/cuda-12.8/include scripts/p2p_benchmark.c -o p2p_benchmark \
    -L/usr/local/cuda-12.8/lib64 -lcudart
LD_LIBRARY_PATH=/usr/local/cuda-12.8/lib64 ./p2p_benchmark
```

Ожидаемый вывод (эталон ai100gb):

```
H2D reference : 2.75 GB/s
peer-async 0->1: 2.47 GB/s
peer-async 1->0: 2.47 GB/s
integrity 0->1 : 0 mismatches
integrity 1->0 : 0 mismatches
host RAM      : 0 mismatches
RESULT: PASS
```

Критично мерять именно **`cudaMemcpyPeerAsync`** (async, реальный DMA-путь):

- Синхронный `cudaMemcpyPeer` стаджит копию через host RAM (~1.5 GB/s, ≈ половина линка) —
  его «no error» НЕ доказывает прямой DMA.
- async-путь даёт реальный BAR1 DMA (~2.47 GB/s ≈ 90% от H2D-эталона).
- Драйверный API (`cuMemcpyPeerAsync`) руками через ctypes НЕ гонять: `cuMemAlloc` аллоцирует
  на устройстве *текущего* контекста — если source и dest аллоцировать при одном текущем
  контексте, обе попадут на одну карту и peer-copy падает `INVALID_CONTEXT` (артефакт
  маршалинга, а не отказ драйвера).

---

## 8. Безопасность (что НЕ делать)

- **Порядок включения BAR1 ПЕРЕД форком.** Если `pcieP2PType=BAR1`, но статическая BAR1 не
  включена, драйвер деградирует в mailbox-fallback (`kbusIsPcieBar1P2PMappingSupported` =
  FALSE → connectivity = PCIE_PROPRIETARY), а mailbox-путь запускается с незарегистрированным
  peer-состоянием (пре-регистрация мертва: `gpumgrGetGpuLinkCount ≡ 0`). Под `iommu=pt`
  битый DMA **тихо портит RAM хоста** (ассерты `remoteWMBoxLocalAddr != ~0ULL` в
  `kern_bus_gm200.c`, затем GPF в произвольных процессах, повреждённый journal). Поэтому:
  статическая BAR1 (32 GiB) должна быть поднята **до** загрузки форка.
- **iommu=pt обязателен** для работы BAR1 P2P, но он же превращает любой mapping-баг в тихую
  порчу — гонять тест только с content-verifying integrity-check, только доверенным софтом,
  при физическом доступе к питанию.
- **Rescan после того как `nvidia` открыл карту не делать** — второй GSP boot может упасть
  (`RmInitAdapter`) и оставить сервис в ожидании. Порядок: модули вне early boot →
  remove/rescan → `nvidia` ровно один раз → ждать GSP/nvidia-smi → retrain только линков.
- **Перед ребутом** остановить `cmp50hx-gen2-rescan.service` (`sudo systemctl stop
  cmp50hx-gen2-rescan && sudo systemctl disable cmp50hx-gen2-rescan`) — иначе shutdown висит
  15–20 минут. После загрузки — `enable --now`. (`systemctl mask` падает с `File already
  exists` — юнит реальный файл, не симлинк.)
- **`cmp50_rebar_size=0`** — иначе драйверный OS-side ReBAR-путь валит BAR2 self-test
  (`kbusVerifyBar2_GM107 garbage 0x0`).

---

## 9. Recovery (если POST висит)

Признак POST-останова (не зависшей ОС): на заведомо рабочей видеокарте чёрный экран,
вентиляторы крутятся, LAN/SSD светятся, пинг без ответа.

По возрастанию сложности:

1. Полный сброс CMOS (БП в 0, батарейка/CLR_CMOS на 30–60 с). На Aptio V сброс CMOS **не
   гарантированно** стирает UEFI-переменные в SPI — шаг вероятностный.
2. Вынуть CMP 50HX, оставить видеокарту: без карт никто не просит 64+ GiB MMIO, POST
   пройдёт, и из ОС перепрошивается образ (`flashrom -w`), чья запись перезаписывает и
   NVAR-том с плохой переменной.
3. Внешняя прошивка SPI (CH341A + SOIC8).

`XveBar1Selector` лежит в SPI NVRAM (не в батарейном CMOS) — «машина поднялась без карт»
НЕ означает «переменная в порядке»; проверить её (`od -An -tu1 <efivar>`, норма `7 0 0 0 8`
или `… 9`) до возврата карт.

---

## 10. Репозиторий-зеркало исследования

Публичное зеркало: <https://github.com/FASTCHIP/cmp50hx_20GB_X16_BTC79X5>. Здесь же лежат:

- `firmware/` — образы (базовый и `-mmioh256g`), исходники DXE pre-pass, патч-скрипт.
- `scripts/` — Gen2-сервисы и P2P-бенчмарк (идентичны развёрнутым на ai100gb).
- `docs/` — детальные разборы (ReBAR/MMIOH, BAR1-P2P, hard rules).
