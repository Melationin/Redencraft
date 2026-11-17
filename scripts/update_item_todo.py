"""Refresh the official item inventory in ITEM_TODO.md.

The inventory is a review checklist, not an assertion that any entry has no
implementation. Partial implementations stay listed until individually audited.
"""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
todo = root / "ITEM_TODO.md"
start = "<!-- official-item-inventory:start -->"
end = "<!-- official-item-inventory:end -->"
names = []
for line in (root / "assets/protocol775/items.tsv").read_text(encoding="utf-8").splitlines():
    if not line.strip():
        continue
    _, name = line.split("\t", 1)
    names.append(name)
if len(names) != len(set(names)):
    raise SystemExit("duplicate item name in official 26.1 export")
text = todo.read_text(encoding="utf-8")
if text.count(start) != 1 or text.count(end) != 1:
    raise SystemExit("ITEM_TODO.md inventory markers must each appear once")
before, after = text.split(start, 1)
_, after = after.split(end, 1)
rows = [
    start,
    "",
    f"共 {len(names)} 个原版物品，顺序与 26.1 官方导出一致。",
    "该清单表示仍需逐个核对基础行为，不代表每项都完全没有实现。",
    "",
]
for offset in range(0, len(names), 8):
    rows.append("- " + "、".join(f"`{name}`" for name in names[offset : offset + 8]))
rows += ["", end]
todo.write_text(before + "\n".join(rows) + after, encoding="utf-8")
print(f"updated {len(names)} official item entries")
