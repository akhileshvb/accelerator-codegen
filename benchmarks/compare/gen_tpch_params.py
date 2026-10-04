#!/usr/bin/env python3
"""TPC-H substitution parameters (spec 2.4, qgen ranges) for comparing the C++
accelerators (accel_tpch_qN_aux) and the Verus kits (accel_tpch_qN_vgen).

Each parameter set is drawn once and rendered into both argument lists, so both
implementations answer the same question.  Where a Verus kit fixes a parameter
as a literal in its SQL (sql2verus takes only literal LIKE patterns), the C++
argument is set to that literal; where the C++ accelerator derives an end date,
the Verus kit takes it explicitly.

    python3 benchmarks/compare/gen_tpch_params.py [sets=8] [seed=20261004] > benchmarks/compare/tpch_params.json
"""
import datetime as dt
import json
import random
import sys

SETS = int(sys.argv[1]) if len(sys.argv) > 1 else 8
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 20261004
R = random.Random(SEED)

NATIONS = {  # name -> region
    "ALGERIA": "AFRICA", "ARGENTINA": "AMERICA", "BRAZIL": "AMERICA", "CANADA": "AMERICA", "EGYPT": "MIDDLE EAST",
    "ETHIOPIA": "AFRICA", "FRANCE": "EUROPE", "GERMANY": "EUROPE", "INDIA": "ASIA", "INDONESIA": "ASIA",
    "IRAN": "MIDDLE EAST", "IRAQ": "MIDDLE EAST", "JAPAN": "ASIA", "JORDAN": "MIDDLE EAST", "KENYA": "AFRICA",
    "MOROCCO": "AFRICA", "MOZAMBIQUE": "AFRICA", "PERU": "AMERICA", "CHINA": "ASIA", "ROMANIA": "EUROPE",
    "SAUDI ARABIA": "MIDDLE EAST", "VIETNAM": "ASIA", "RUSSIA": "EUROPE", "UNITED KINGDOM": "EUROPE",
    "UNITED STATES": "AMERICA"}
REGIONS = ["AFRICA", "AMERICA", "ASIA", "EUROPE", "MIDDLE EAST"]
SEGMENTS = ["AUTOMOBILE", "BUILDING", "FURNITURE", "MACHINERY", "HOUSEHOLD"]
SHIPMODES = ["REG AIR", "AIR", "RAIL", "SHIP", "TRUCK", "MAIL", "FOB"]
TYPE1 = ["STANDARD", "SMALL", "MEDIUM", "LARGE", "ECONOMY", "PROMO"]
TYPE2 = ["ANODIZED", "BURNISHED", "PLATED", "POLISHED", "BRUSHED"]
TYPE3 = ["TIN", "NICKEL", "BRASS", "STEEL", "COPPER"]
CONT1 = ["SM", "LG", "MED", "JUMBO", "WRAP"]
CONT2 = ["CASE", "BOX", "BAG", "JAR", "PKG", "PACK", "CAN", "DRUM"]


def d(x):
    return f"DATE '{x.isoformat()}'"


def add_months(x, n):
    m = x.month - 1 + n
    return dt.date(x.year + m // 12, m % 12 + 1, 1)


def month_in(lo, hi):  # first day of a random month in [lo, hi]
    months = (hi.year - lo.year) * 12 + hi.month - lo.month
    return add_months(lo, R.randint(0, months))


def jan1():
    return dt.date(R.randint(1993, 1997), 1, 1)


def brand():
    return f"Brand#{R.randint(1, 5)}{R.randint(1, 5)}"


def q(s):
    return "'" + s.replace("'", "''") + "'"


def one(n):
    """(params, cpp args, verus args, note) for query n."""
    if n == 1:
        delta = R.randint(60, 120)
        return {"delta": delta}, f"{delta}", f"{delta}", ""
    if n == 2:
        size, region = R.randint(1, 50), R.choice(REGIONS)
        return ({"size": size, "region": region}, f"{size}, '%BRASS', {q(region)}", f"{size}, {q(region)}",
                "type pattern fixed to '%BRASS' (Verus literal)")
    if n == 3:
        seg, day = R.choice(SEGMENTS), dt.date(1995, 3, R.randint(1, 31))
        return {"segment": seg, "date": str(day)}, f"{q(seg)}, {d(day)}", f"{q(seg)}, {d(day)}", ""
    if n == 4:
        day = month_in(dt.date(1993, 1, 1), dt.date(1997, 10, 1))
        return {"date": str(day)}, d(day), f"{d(day)}, {d(add_months(day, 3))}", ""
    if n == 5:
        region, day = R.choice(REGIONS), jan1()
        return ({"region": region, "date": str(day)}, f"{q(region)}, {d(day)}",
                f"{q(region)}, {d(day)}, {d(day.replace(year=day.year + 1))}", "")
    if n == 6:
        day, disc, qty = jan1(), R.randint(2, 9) / 100, R.randint(24, 25)
        return ({"date": str(day), "discount": disc, "quantity": qty}, f"{d(day)}, {disc:.2f}, {qty}",
                f"{d(day)}, {disc:.2f}, {qty}.00, {d(day.replace(year=day.year + 1))}", "")
    if n == 7:
        n1, n2 = R.sample(sorted(NATIONS), 2)
        return {"nation1": n1, "nation2": n2}, f"{q(n1)}, {q(n2)}", f"{q(n1)}, {q(n2)}", ""
    if n == 8:
        nation = R.choice(sorted(NATIONS))
        ptype = f"{R.choice(TYPE1)} {R.choice(TYPE2)} {R.choice(TYPE3)}"
        args = f"{q(nation)}, {q(NATIONS[nation])}, {q(ptype)}"
        return {"nation": nation, "type": ptype}, args, args, ""
    if n == 9:
        return {"color": "green"}, "'green'", "", "color fixed to 'green' (Verus literal)"
    if n == 10:
        day = month_in(dt.date(1993, 2, 1), dt.date(1995, 1, 1))
        return {"date": str(day)}, d(day), f"{d(day)}, {d(add_months(day, 3))}", ""
    if n == 11:
        nation = R.choice(sorted(NATIONS))
        return ({"nation": nation}, f"{q(nation)}, 0.0001", q(nation),
                "fraction fixed to 0.0001 (Verus literal; spec: 0.0001/SF)")
    if n == 12:
        m1, m2 = R.sample(SHIPMODES, 2)
        day = jan1()
        return ({"shipmode1": m1, "shipmode2": m2, "date": str(day)}, f"{q(m1)}, {q(m2)}, {d(day)}",
                f"{q(m1)}, {q(m2)}, {d(day)}, {d(day.replace(year=day.year + 1))}", "")
    if n == 13:
        return ({"pattern": "%unusual%accounts%"}, "'%unusual%accounts%'", "",
                "pattern fixed to '%unusual%accounts%' (Verus literal)")
    if n == 14:
        day = month_in(dt.date(1993, 1, 1), dt.date(1997, 12, 1))
        return {"date": str(day)}, d(day), f"{d(day)}, {d(add_months(day, 1))}", ""
    if n == 15:
        day = month_in(dt.date(1993, 1, 1), dt.date(1997, 10, 1))
        return {"date": str(day)}, d(day), f"{d(day)}, {d(add_months(day, 3))}", ""
    if n == 16:
        b, sizes = brand(), R.sample(range(1, 51), 8)
        s = ", ".join(map(str, sizes))
        return ({"brand": b, "sizes": sizes}, f"{q(b)}, 'SMALL BRUSHED%', {s}", f"{q(b)}, {s}",
                "type prefix fixed to 'SMALL BRUSHED%' (Verus literal)")
    if n == 17:
        b, c = brand(), f"{R.choice(CONT1)} {R.choice(CONT2)}"
        return {"brand": b, "container": c}, f"{q(b)}, {q(c)}", f"{q(b)}, {q(c)}", ""
    if n == 18:
        qty = R.randint(312, 315)
        return {"quantity": qty}, f"{qty}", f"{qty}", ""
    if n == 19:
        b = [brand() for _ in range(3)]
        qs = [R.randint(1, 10), R.randint(10, 20), R.randint(20, 30)]
        args = ", ".join(f"{q(b[i])}, {qs[i]}" for i in range(3))
        return {"brands": b, "quantities": qs}, args, args, ""
    if n == 20:
        day, nation = jan1(), R.choice(sorted(NATIONS))
        return ({"date": str(day), "nation": nation}, f"{d(day)}, 'gainsboro%', {q(nation)}",
                f"{d(day)}, {q(nation)}, {d(day.replace(year=day.year + 1))}",
                "part name prefix fixed to 'gainsboro%' (Verus literal)")
    if n == 21:
        nation = R.choice(sorted(NATIONS))
        return {"nation": nation}, q(nation), q(nation), ""
    if n == 22:
        codes = [str(c) for c in R.sample(range(10, 35), 7)]
        args = ", ".join(q(c) for c in codes)
        return {"codes": codes}, args, args, ""
    raise ValueError(n)


out = {"seed": SEED, "queries": {}}
for n in range(1, 23):
    sets, seen = [], set()
    for _ in range(SETS * 4):
        params, cpp, verus, note = one(n)
        if cpp in seen:
            continue
        seen.add(cpp)
        sets.append({"params": params, "cpp": cpp, "verus": verus})
        if len(sets) == SETS:
            break
    out["queries"][str(n)] = {"cpp_id": f"tpch:{n}", "verus_id": f"tpch_q{n}_vgen", "note": note, "sets": sets}
json.dump(out, sys.stdout, indent=1)
print()
