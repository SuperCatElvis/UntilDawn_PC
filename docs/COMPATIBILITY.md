Until Dawn PC Recompilation - game editions (v0.5.0)
=====================================================

You need your own copy of Until Dawn (2015) for PS4, extracted, with update 01.03 applied. No game files are
included with this program.

Which editions work
-------------------
This program runs the game's own 01.03 program code and changes it at fixed places. It therefore checks, at every
start, that your eboot.bin is exactly that 01.03 program. Harmless regional differences (the signature, a build date
and a build number) are allowed; anything else is refused with an explanation, never started.
| Title ID | Edition | Status | Languages in the edition |
|---|---|---|---|
| CUSA02636 | Europe 2 (EP9000) | **SUPPORTED** | English, Spanish, Portuguese, Russian, Polish (text + voices); Danish, Finnish, Norwegian, Swedish (text) |
| CUSA00194 | Europe 1 (EP9000) | **SUPPORTED** | English, French, German, Italian (text + voices); Dutch, Turkish (text) |
| CUSA00359 | Americas (UP9000: US, Canada, Latin America incl. Brazil) | **EXPERIMENTAL** | English, French, Latin American Spanish, Brazilian Portuguese (file list; not tested) |
| CUSA01913 | UK | **UNTESTED** | English |
| CUSA00902 | Japan | **UNTESTED** | Japanese (2) |
| CUSA01423 | Asia (English/Chinese/Korean) | **UNTESTED** | English, Korean, Chinese (Traditional and Simplified) |
| CUSA02915 | Unknown (listed in the game's code) | **UNTESTED** | — |

### Compatibility Status

- **SUPPORTED:** Tested by us in game with update 01.03.
- **EXPERIMENTAL:** Accepted when its `eboot.bin` passes the program check; its file list is known, but we have not tested it.
- **UNTESTED:** Known title, but nobody has measured its files. It starts only if its `eboot.bin` passes the program check; then only the files all editions share are checked. Please report how it goes.

### Additional Notes

**(1)** CUSA00194 01.03's program is identical to CUSA02636 01.03 except for 59 bytes of signature and build data (measured). Tested for v0.5.0: start-up check, Chapter 1, the prologue in French, and Russian imported from CUSA02636. Its game folder has an empty `title-id.txt`; the program answers the game's request for it with `CUSA00194_00` (the game's own default) — the file itself is not changed.

**(2)** The Japanese edition's other game data differs (it was censored), so only its language files are likely to be usable with another edition.

Version 01.00 (without the update) of any edition is refused: apply the 01.03 update first.

If your edition is refused
--------------------------
The message and Logs\UntilDawn.log show the title, version, SHA-256 and "code fingerprint" of your eboot.bin. If it
is an unmodified 01.03 dump, please open a GitHub issue with those lines and an Inventory report from
UDLanguageImporter.exe (file names, sizes and hashes only - no game data).

Languages from another edition
------------------------------
UDLanguageImporter.exe copies the language files of a SECOND edition you own into the Languages folder, once,
whichever edition your game folder is: a CUSA00194 player can add Spanish, Portuguese, Russian, Polish and the Nordic
languages from CUSA02636, and a CUSA02636 player can add French, German, Italian, Dutch and Turkish from CUSA00194.
Languages already in your own edition are recognised and not imported again. Then set  language = <name or code>  in
Config\settings.ini (the file lists all 20 languages, their codes, editions and whether they have voices).
