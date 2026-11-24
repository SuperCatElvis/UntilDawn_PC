Until Dawn PC Recompilation - extra languages from a second PS4 edition you own (optional)

Your game folder is CUSA02636 or CUSA00194. Each of these European editions has its own languages; the other edition's
languages can be added once from your own copy of it. Your game folder is never changed.

  edition     languages (VERIFIED)
  CUSA00194   French, German, Italian (subtitles + voices); Dutch, Turkish (subtitles; voices stay English)
  CUSA02636   Spanish, Portuguese, Russian, Polish (subtitles + voices); Danish, Finnish, Norwegian, Swedish (subtitles)

Languages already in your own edition are recognised and are not imported again.

1. Extract the second edition (game) and its 01.03 update into two folders, e.g. D:\PS4\CUSA00194 and
   D:\PS4\CUSA00194-patch. (A folder that already has the update applied can be given twice.)
2. EASY WAY: start UDLanguageImporter.exe (next to UntilDawn.exe), pick the game folder (the update folder is found when
   it sits next to it), keep "All languages" ticked and press Install. Or, on the command line, in the folder of
   UntilDawn.exe run (update folder first):
       ud_langscan --install all --from D:\PS4\CUSA00194-patch D:\PS4\CUSA00194
   (or e.g. --install fr,de). Every file is checked; only complete languages are copied, into Languages\<code>\
   (a dubbed language about 450-560 MB, a subtitles-only language a few MB), and a report ud_langscan_report.txt is written.
3. Config\settings.ini:   language = french   (or the code: fr). The file lists all 20 languages and their codes.
4. Afterwards you may delete the second edition's folders. Other commands:
       ud_langscan --list [--verify full]      state of each language
       ud_langscan --repair fr --from ...      fix a damaged language (re-uses its good files)
       ud_langscan --remove fr                 delete one language
   If a language is incomplete or damaged the game starts in English and says why in Logs\UntilDawn.log.

OTHER EDITIONS (Americas CUSA00359, Asia CUSA01423, Japan CUSA00902, ...) are NOT verified. Help us by sending an
inventory (file names, sizes and hashes only - no game data):
       ud_langscan --inventory <update folder> <game folder> --json
   and attach ud_langscan_inventory.json to a GitHub issue ("Language inventory" template).
Experimental import (at your own risk; may show wrong text, no voices, or crash):
       ud_langscan --install-experimental <update folder> <game folder> --i-accept-unverified-files
   goes into Languages\experimental\<edition>\all\ and is used ONLY with the settings
       language_experimental = <edition>\all        text_language = <language>
   Remove it with  ud_langscan --remove-experimental <edition>.  Verified languages are never touched.
