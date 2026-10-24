#!/usr/bin/env python3
"""Writes import_people.xlsx, the workbook tst_externaldata and the live
--importexternal selftests read: a sheet with every kind of cell
ExternalData's reader handles (shared, rich-text and inline strings, numbers
Excel writes with 17 digits, booleans, cells formatted as a date and as a
date and time, an empty cell and a row with gaps) and a second sheet.
Written by hand with the standard library, no Excel needed:

    python3 tests/fixtures/make_import_xlsx.py tests/fixtures/import_people.xlsx
"""
import sys
import zipfile
from datetime import date

def serial(d):  # Excel's 1900 date system
    return (d - date(1899, 12, 30)).days

FILES = {
    "[Content_Types].xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
<Override PartName="/xl/worksheets/sheet2.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>
<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>
</Types>""",
    "_rels/.rels": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>""",
    "xl/workbook.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
<workbookPr/>
<sheets><sheet name="People" sheetId="1" r:id="rId1"/><sheet name="Other" sheetId="2" r:id="rId2"/></sheets>
</workbook>""",
    "xl/_rels/workbook.xml.rels": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="/xl/worksheets/sheet2.xml"/>
<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>
<Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>""",
    # style 1: a built-in date format (14); 2: a custom date and time format
    "xl/styles.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
<numFmts count="1"><numFmt numFmtId="164" formatCode="yyyy\\-mm\\-dd hh:mm:ss"/></numFmts>
<cellXfs count="3"><xf numFmtId="0"/><xf numFmtId="14"/><xf numFmtId="164"/></cellXfs>
</styleSheet>""",
    "xl/sharedStrings.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="9" uniqueCount="9">
<si><t>id</t></si><si><t>name</t></si><si><t>salary</t></si><si><t>joined</t></si>
<si><t>active</t></si><si><t>note</t></si>
<si><t>Alice</t></si>
<si><r><t>রহি</t></r><r><rPr><b/></rPr><t>ম</t></r><rPh><t>x</t></rPh></si>
<si><t xml:space="preserve">a, "quoted" note </t></si>
</sst>""",
    "xl/worksheets/sheet1.xml": f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
<dimension ref="A1:F5"/>
<sheetData>
<row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="s"><v>1</v></c><c r="C1" t="s"><v>2</v></c><c r="D1" t="s"><v>3</v></c><c r="E1" t="s"><v>4</v></c><c r="F1" t="s"><v>5</v></c></row>
<row r="2"><c r="A2"><v>1</v></c><c r="B2" t="s"><v>6</v></c><c r="C2"><v>5000.5</v></c><c r="D2" s="1"><v>{serial(date(2024, 1, 15))}</v></c><c r="E2" t="b"><v>1</v></c><c r="F2" t="s"><v>8</v></c></row>
<row r="3"><c r="A3"><v>2</v></c><c r="B3" t="s"><v>7</v></c><c r="C3"><v>4200</v></c><c r="D3" s="2"><v>{serial(date(2023, 6, 1))}.4375</v></c><c r="E3" t="b"><v>0</v></c><c r="F3" t="inlineStr"><is><t>inline text</t></is></c></row>
<row r="4"><c r="A4"><v>3</v></c><c r="B4" t="str"><v>Dana</v></c><c r="C4"><v>0.10000000000000001</v></c><c r="E4" t="b"><v>1</v></c></row>
<row r="5"><c r="A5"><v>4</v></c><c r="C5"><v>1.5E3</v></c></row>
</sheetData>
</worksheet>""",
    "xl/worksheets/sheet2.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
<sheetData><row r="1"><c r="A1" t="inlineStr"><is><t>a</t></is></c></row><row r="3"><c r="A3"><v>7</v></c></row></sheetData>
</worksheet>""",
}

with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    for name, text in FILES.items():
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_STORED if name.startswith("_rels") else zipfile.ZIP_DEFLATED
        z.writestr(info, text.encode("utf-8"))
