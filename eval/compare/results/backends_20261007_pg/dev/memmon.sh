#!/bin/bash
# every 10 s: host used MiB, container anon + shmem MiB (anon = backends' private memory, incl. accelerator state)
CG=$1; LOG=$2
echo "time host_used_mib host_avail_mib pg_anon_mib pg_shmem_mib" >> "$LOG"
while true; do
  read -r used avail < <(free -m | awk '/^Mem:/{print $3, $7}')
  anon=$(awk '$1=="anon"{print int($2/1048576)}' $CG/memory.stat); shm=$(awk '$1=="shmem"{print int($2/1048576)}' $CG/memory.stat)
  echo "$(date +%FT%T) $used $avail $anon $shm" >> "$LOG"; sleep 10
done
