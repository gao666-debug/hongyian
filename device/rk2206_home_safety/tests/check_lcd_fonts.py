"""Check every Chinese UI label has a 16px and 24px glyph."""
from pathlib import Path
import re
p=Path(__file__).resolve().parents[1]
texts=''.join(re.findall(r'"([^"\n]*)"',(p/'src/ui.c').read_text()+(p/'src/living.c').read_text()))
chars=set(re.findall('[\u4e00-\u9fff]',texts))
font=(p/'include/lcd_font.h').read_text()
for size in (16,24):
    begin=font.index(' tfont%d[]'%size);table=font[begin:font.index('};',begin)]
    normalized=''.join(table.split()).lower()
    missing=[ch for ch in chars if '"'+ch+'"' not in table and ','.join('0x%02x'%b for b in ch.encode()) not in normalized]
    assert not missing,(size,missing)
print('PASS Chinese glyph coverage: %d characters, both 16px and 24px'%len(chars))
