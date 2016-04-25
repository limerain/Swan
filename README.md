﻿# Swan
Swan is gaurantee maximum bandwidth of SSD Array(or set of SSDs)

#한글/Korean
Swan(가칭)는 다중 SSD환경을 고려하여 SSD들의 최대 대역폭을 보장해주는 관리 기법입니다.
v0.x 버전은 기본적인 동작을 구현중이며 아직 버그가 있습니다.

리눅스의 커널 버전에 크게 구애받지 않으며, dm-stripe.c 코드를
/drivers/md/dm-stripe.c와 교체해주시면 됩니다.
커널 컴파일 한 후, LVM2 명령을 이용해서 생성해주시면 됩니다.

아직은 LVM2의 소스 수정을 통해 Swan만의 생성 명령을 만들지 못했습니다.
따라서 소스 코드명도 dm-stripe.c이며, 기존의 Stripe LVM을 생성하듯 만들어주시면 됩니다.
다음은 예시입니다.
```
pvcreate /dev/sdb1
pvcreate /dev/sdb2
pvcreate /dev/sdc1
pvcreate /dev/sdc2
vgcreate Swan /dev/sdb1 /dev/sdb2 /dev/sdc1 /dev/sdc2
lvcreate -i 4 -I 4 -l 100%FREE -n Swan Swan
```
`lvcreate -i N`
에서 N은 SSD의 개수입니다.

물리적으로 다른 SSD를 요구하며 2개 이상 요구되지만, 3개 이상이 최적입니다.
2개일 때는 위와 같이 파티션을 반씩 나눠서 생성해주세요.

--v0.3--
기존의 확인된 에러(중간에 다운되는) 버그를 모두 고쳤습니다.
약 470GB의 디스크를 볼륨화 시킬 수 있습니다.
Array-manager의 이름을 Swan(가칭)으로 변경했습니다.

--v0.2--
v0.1의 에러 중 일부를 고쳤습니다.
fio의 랜덤 쓰기 테스트를 수행 시 600초 가량에서 알 수 없는 시스템 다운이 발생합니다.
100초 내외의 테스트에서 안정적인 수행을 확인했습니다.

--v0.1--
알려진 에러
1. 전체 Array 용량이 크면 에러가 발생합니다.
2. 재현율이 100%가 아닌 General Protection Fault 에러와 함께 실패합니다.

#영어/English
Swan is a manage to guaranteeing SSD's maximum bandwidth in many SSDs environment.
version 0.x series are basic implementation versions.
i'm implementing of basic feature, fix to bug, design new mechanism, and so on.
then version is will up.

this code is not cared linux kernel version.
Swan code file name is dm-stripe.c
it is a replace previously stripe(RAID-0) code. therefore after installed Swan, you can't use stripe.
so construct Swan command is same to construct stripe.
this is Example command.
```
pvcreate /dev/sda1
pvcreate /dev/sdb1
pvcreate /dev/sdb2
vgcreate Swan /dev/sda1 /dev/sdb1 /dev/sdb2
lvcreate -i 3 -I 4 -l 100%FREE -n Swan Swan
```
N is a number of SSD in command `lvcreate -i N`

if you have 2 SSDs, can create Swan by separte partition. but, optimal number is more than 3.

Thnks for your interest.
