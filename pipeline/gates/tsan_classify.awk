# Counts ThreadSanitizer reports as ours (o) or engine-only (e). The engine is
# uninstrumented, so a report is ours when the first non-runtime frame of either
# racing access is a source file matching `pat` (substrate/, backends/, accelerators/).
/^WARNING: ThreadSanitizer/ { inrep = 1; mine = 0; hdr = 0 }
inrep && /^  (Atomic |Previous |Write|Read|Mutex).* by (main )?thread/ { hdr = 1; next }
inrep && hdr && /^    #[0-9]+ / {
  if ($0 ~ /libtsan\.so/) next
  hdr = 0
  if ($0 ~ pat) mine = 1
  next
}
/^SUMMARY: ThreadSanitizer/ { if (mine) o++; else e++; inrep = 0 }
END { print o + 0, e + 0 }
