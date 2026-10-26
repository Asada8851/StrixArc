# План: снять трассу регистров рабочего Linux amdgpu на 780M (на утро)

Цель: узнать, какие записи в регистры (IC_BASE ME/MEC, GRBM_GFX_INDEX, RLC_*, GCVM, CP_*) делает рабочий
драйвер между `PSP AUTOLOAD_RLC` и первым исполнением PM4, и повторить их в PhxBringup.

1. Live-USB с Ubuntu 24.04/25.04 (ядро >= 6.8). Загрузиться с неё на Firebat (OpenCore picker -> USB / F7 boot menu).
2. На загрузочной строке добавить `modprobe.blacklist=amdgpu` (драйвер не должен загрузиться до включения трассировки).
3. Поднять сеть (UE200) и `sudo bash tools/linux-trace/trace.sh` (скрипт лежит в репозитории; можно скопировать по scp).
4. Забрать /tmp/amdgpu_regtrace.txt и /tmp/amdgpu_dmesg.txt.
5. Сравнить порядок записей с нашим bringup3.sh: ищем в трассе CP_PFP_IC_BASE_LO (0x5840+0xA000 абс.), CP_ME_IC_BASE_LO,
   CP_CPC_IC_BASE_LO, GRBM_GFX_INDEX, CP_MEC_RS64_CNTL/CP_MEC_CNTL, RLC_CP_SCHEDULERS и всё что идёт между
   AUTOLOAD_RLC (C2PMSG_64 cmd 0x21) и первой записью CP_RB0/HQD.
Трасса — события amdgpu_device_wreg/rreg (reg в dwords относительно BAR5).
