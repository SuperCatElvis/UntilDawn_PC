package
{
   import flash.display.Bitmap;
   import flash.display.BitmapData;
   import flash.display.MovieClip;
   import flash.display.Sprite;
   import flash.events.Event;
   import flash.external.ExternalInterface;
   import flash.text.TextField;
   import flash.text.TextFieldAutoSize;
   import flash.text.TextFormat;
   import flash.utils.getDefinitionByName;

   /* Ours (Until Dawn PC port, STATUS 86). The movie skip prompt, built like the game's own end-credits "Skip" prompt:
      the Cross button pictures, the localised text SMG013_66585 in MelbourneUD 27 white, right-aligned 50 px from the
      right edge at y 988 of the 1920x1080 stage, fading in over 15 frames and out over 30. The loader adds this class
      to the movie that shows the movie legal line, to the overlay-caption movie (VideoOverlayText) and (t107j) to the
      subtitle movie (Subtitles), and calls UDShowSkipPrompt / UDHideSkipPrompt / UDHideMovieText. A Sprite, not a
      MovieClip: VideoOverlayText sends every MovieClip on its stage to an "off" frame (ResetAll / OnEnterGame) and
      hides them in the menus. */
   public class UDSkipPrompt extends Sprite
   {
      private var box:Sprite;
      private var tf:TextField;
      private var ts:Sprite;
      private var bs:Sprite;
      private var want:Number = 0;
      private var age:int = 0;
      private var shownText:String = null;
      private var pendingText:Boolean = false;
      private var needLayout:Boolean = false;
      private var texts:Object = new Object();

      public function UDSkipPrompt()
      {
         super();
         visible = false;
         alpha = 0;
         addEventListener(Event.ADDED_TO_STAGE, onAdded);
      }

      private function onAdded(e:Event) : void
      {
         removeEventListener(Event.ADDED_TO_STAGE, onAdded);
         var r:Object = root;
         if (r != null)
         {
            r.UDShowSkipPrompt = show;
            r.UDHideSkipPrompt = hide;
            r.UDRemoveSkipPrompt = remove;
            r.UDHideMovieText = hideMovieText;
            r.TextLookupValue = textLookupValue;
         }
         addEventListener(Event.ENTER_FRAME, tick);
         /* t107j: tell the loader which movie (and so which engine instance) carries the prompt: the game's text
            lookup handler gets the instance with the call, so the loader can show the prompt in whichever of its
            host movies is being drawn at the time */
         var url:String = "?";
         try { if (loaderInfo != null && loaderInfo.url != null) url = loaderInfo.url; } catch (err:Error) { }
         report("host " + url);
      }

      public function textLookupValue(... rest) : *
      {
         if (rest.length >= 2) texts[rest[0]] = rest[1];
      }

      private function getText(key:String) : String
      {
         if (texts[key] == null && ExternalInterface.available) ExternalInterface.call("DoTextLookup", key);
         if (texts[key] != null) return "~" + texts[key];
         return key;
      }

      /* The button and the text field are symbols the loader puts in the movie, built like the credits screen's:
         UDSkipButton = the Cross button shape filled with the game's two button pictures (loaded by the engine like
         every other picture of the game's movies); UDSkipText = a MelbourneUD 27 text field named textField. */
      private function build() : void
      {
         if (box != null) return;
         box = new Sprite();
         box.x = 1920;
         box.y = 988;
         addChild(box);
         ts = makeSymbol("UDSkipText");
         if (ts != null) { try { tf = Object(ts).textField as TextField; } catch (err:Error) { report("text field: " + err.message); } box.addChild(ts); }
         bs = makeSymbol("UDSkipButton");
         if (bs != null) box.addChild(bs);
      }

      private function makeSymbol(cls:String) : Sprite
      {
         var c:Class = null;
         try { c = getDefinitionByName(cls) as Class; } catch (err:Error) { report("no class " + cls + ": " + err.message); return null; }
         try { return new c() as Sprite; } catch (err2:Error) { report("symbol " + cls + ": " + err2.message); }
         return null;
      }

      /* the text (looked up again each time until the game has answered) and the layout of the game's button prompts:
         from the right edge, 50 px space, the text, then the button */
      private function layout() : void
      {
         var baseX:Number = -50;
         if (tf != null)
         {
            /* set through "" so the text is laid out again: the letters are looked up in Scaleform's glyph cache anew
               (t97: shown again later, the old letters were white blocks - their cache slots had been reused) */
            var t:String = getText("SMG013_66585");
            tf.text = "";
            tf.text = t;
            shownText = t;
            baseX -= tf.textWidth;
         }
         if (ts != null) ts.x = baseX;
         baseX -= 52.25;
         if (bs != null) bs.x = baseX;
      }

      /* The game calls these from outside the movie (eboot+0x14e3500). They only set flags: the work (building the
         prompt, asking the game for the localised text) is done in the next frame of the movie, the way the game's own
         prompts do it, never inside the game's call. */
      public function show() : *
      {
         want = 1;
         needLayout = true;
      }

      public function hide() : *
      {
         want = 0;
      }

      /* gone at once (the loader sends this a second after the fade-out started, in case the movie is not being
         advanced any more and the fade never runs) */
      public function remove() : *
      {
         want = 0;
         alpha = 0;
         visible = false;
      }

      public function hideMovieText() : *
      {
         pendingText = true;
      }

      private function report(what:String) : void
      {
         try { if (ExternalInterface.available) ExternalInterface.call("DoTextLookup", "UDLOG " + what); } catch (err:Error) { }
      }

      private function tick(e:Event) : void
      {
         if (pendingText)
         {
            pendingText = false;
            var r:Object = root;
            try { if (r != null && r.top != null) r.top.gotoAndStop("off"); } catch (err:Error) { report("legal off: " + err.message); }
            try { if (r != null && r.top != null && r.top.text != null) r.top.text.textField.text = ""; } catch (err2:Error) { report("legal text: " + err2.message); }
            try { if (r != null) r.isDone = true; } catch (err3:Error) { }
         }
         if (needLayout)
         {
            needLayout = false;
            try { build(); } catch (err4:Error) { report("build: " + err4.message); }
            try { layout(); } catch (err5:Error) { report("layout: " + err5.message); }
            if (box != null) report("shown, text width " + (tf != null ? tf.textWidth : -1) + ", pictures " + (bs != null ? bs.numChildren : -1));
            visible = true;
         }
         /* keep the letters fresh (see layout) - with the string we set: reading tf.text back gives the translated
            text, which the game's translation then does not know (t99: "Skip" vanished after the first refresh) */
         if (visible && tf != null && shownText != null && ++age % 20 == 0) { tf.text = ""; tf.text = shownText; }
         if (want > 0 && alpha < 1) alpha = Math.min(1, alpha + 1 / 15);
         else if (want == 0 && alpha > 0) alpha = Math.max(0, alpha - 1 / 30);
         if (alpha <= 0 && want == 0) visible = false;
      }
   }
}
