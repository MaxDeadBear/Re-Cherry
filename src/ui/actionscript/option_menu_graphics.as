// Appended to the options screen controller by src/ui/option_menu.cpp: Y, LB or RB flips its seven rows to a GRAPHICS page whose changes apply at once through CherryGfx_* calls (docs/ui/option-menu.md).

var cherryMc = this;
var cherryPage = 0;
var cherryRowCursor = [0, 1, 2, 8, 3, 4, 9];
var cherryFocusNames = ["Vibration", "CameraUpDown", "CameraRL", "Aiming", "BGMVol", "SEVol", "Subtitle"];
var cherryHits = ["hit_vib", "hit_camud", "hit_camrl", "hit_aim", "hit_bgm", "hit_se", "hit_sub"];
var cherryOrigLabels = [but_vib, but_camud, but_camrl, but_aim, but_bgm, but_se, but_sub];
var cherryOrigValues = [vib_vol, camud_vol, camrl_vol, aim_vol, bgm_vol, se_vol, sub_vol];
var cherryNames = ["RESOLUTION", "ANTI-ALIASING", "TEXTURE FILTERING", "ASPECT RATIO", "DISPLAY MODE", "V-SYNC", "FRAME RATE"];
var cherryChoices = [["AUTO", "144P", "360P", "720P", "1080P", "1440P", "2160P"], ["OFF", "FXAA"], ["DEFAULT", "2X", "4X", "8X", "16X"], ["ORIGINAL", "STRETCH"], ["WINDOWED", "FULLSCREEN"], ["OFF", "ON"], ["30 FPS", "60 FPS", "UNCAPPED"]];
var cherryValue = [];
var cherryBefore = [];
var cherryLabels = [];
var cherryValues = [];

function cherryField(mc)
{
   if(mc == undefined)
   {
      return undefined;
   }
   if(mc.str != undefined)
   {
      return mc.str;
   }
   var inner = mc.getInstanceAtDepth(1);
   if(inner.str != undefined)
   {
      return inner.str;
   }
   for(var k in mc)
   {
      if(typeof mc[k] == "movieclip")
      {
         var found = cherryField(mc[k]);
         if(found != undefined)
         {
            return found;
         }
      }
   }
   return undefined;
}

function cherrySetText(field, text, align)
{
   if(field != undefined && field.text != text)
   {
      field.autoSize = align;
      field.wordWrap = false;
      field.text = text;
   }
}

function cherryRefresh()
{
   if(cherryPage == 0)
   {
      return undefined;
   }
   var i = 0;
   while(i < 7)
   {
      cherrySetText(cherryField(cherryLabels[i]), cherryNames[i], "left");
      cherrySetText(cherryField(cherryValues[i].cel), cherryChoices[i][cherryValue[i]], "center");
      i++;
   }
}

function cherryShow(i)
{
   cherryValues[i].SetChangeMode(cherryValue[i] != 0);
   cherryRefresh();
}

function cherryRowOfCursor()
{
   var i = 0;
   while(i < 7)
   {
      if(cherryRowCursor[i] == mSelectCursor)
      {
         return i;
      }
      i++;
   }
   return -1;
}

function cherryStep(i, delta)
{
   var n = cherryChoices[i].length;
   cherryValue[i] = (cherryValue[i] + delta + n) % n;
   cherryShow(i);
   flash.external.ExternalInterface.call("CherryGfx_Set", i, cherryValue[i]);
}

function cherryLoad()
{
   var i = 0;
   while(i < 7)
   {
      var v = Number(flash.external.ExternalInterface.call("CherryGfx_Get", i));
      if(isNaN(v) || v < 0 || v >= cherryChoices[i].length)
      {
         v = 0;
      }
      cherryValue[i] = v;
      cherryBefore[i] = v;
      i++;
   }
}

function cherrySetPage(page)
{
   var row = cherryRowOfCursor();
   if(row >= 0)
   {
      cherryMc["MovUnFocus_" + cherryFocusNames[row]]();
   }
   cherryPage = page;
   var i = 0;
   while(i < 7)
   {
      cherryOrigLabels[i]._visible = page == 0;
      cherryOrigValues[i]._visible = page == 0;
      cherryLabels[i]._visible = page == 1;
      cherryValues[i]._visible = page == 1;
      if(page == 1)
      {
         cherryShow(i);
      }
      i++;
   }
   cherrySetText(cherryHint, page == 0 ? "Y  GRAPHICS     X  FPS COUNTER" : "Y  GAME     X  FPS COUNTER", "left");
   if(row >= 0)
   {
      bSeInit = false;
      cherryMc["MovFocus_" + cherryFocusNames[row]]();
   }
}

function cherryMakeFocus(i)
{
   var original = cherryMc["MovFocus_" + cherryFocusNames[i]];
   return function()
   {
      if(cherryPage == 0)
      {
         original();
         return undefined;
      }
      mSelectCursor = cherryRowCursor[i];
      cherryLabels[i].gotoAndPlay("focus");
      cherryValues[i].gotoAndPlay("focus_in");
      cherryRefresh();
      if(bSeInit)
      {
         flash.external.ExternalInterface.call("PlayMenuSoundCue", 5);
      }
      else
      {
         bSeInit = true;
      }
   };
}

function cherryMakeUnFocus(i)
{
   var original = cherryMc["MovUnFocus_" + cherryFocusNames[i]];
   return function()
   {
      if(cherryPage == 0)
      {
         original();
         return undefined;
      }
      cherryLabels[i].gotoAndPlay("def");
      cherryValues[i].gotoAndPlay("def");
      cherryRefresh();
   };
}

function cherryMakeOut(i, original)
{
   return function()
   {
      if(cherryPage == 0)
      {
         original();
         return undefined;
      }
      if(mSelectCursor == cherryRowCursor[i])
      {
         cherryStep(i, cherryMc[cherryHits[i] + "_2"].focused ? -1 : 1);
      }
      cherryMc[cherryHits[i]].focused = true;
   };
}

function cherryWrapOut(i, name)
{
   cherryMc[name] = cherryMakeOut(i, cherryMc[name]);
}

var cherryOrigSelect = SelectButton_Push;
SelectButton_Push = function()
{
   var row = cherryRowOfCursor();
   if(cherryPage == 1 && bPush == false && row >= 0)
   {
      flash.external.ExternalInterface.call("PlayMenuSoundCue", 1);
      cherryStep(row, 1);
      cherryValues[row].gotoAndPlay("push");
      return undefined;
   }
   cherryOrigSelect();
};

var cherryOrigSetting = AllOptionSetting;
AllOptionSetting = function()
{
   cherryOrigSetting();
   var i = 0;
   while(i < 7)
   {
      flash.external.ExternalInterface.call("CherryGfx_Set", i, cherryValue[i]);
      i++;
   }
   flash.external.ExternalInterface.call("CherryGfx_Save");
};

var cherryOrigCancel = AllOptionSetCancel;
AllOptionSetCancel = function()
{
   cherryOrigCancel();
   var i = 0;
   while(i < 7)
   {
      cherryValue[i] = cherryBefore[i];
      flash.external.ExternalInterface.call("CherryGfx_Set", i, cherryValue[i]);
      i++;
   }
   cherryRefresh();
};

var cherryOrigDefault = AllOptionSetDefault;
AllOptionSetDefault = function()
{
   if(cherryPage == 0)
   {
      cherryOrigDefault();
      return undefined;
   }
   var i = 0;
   while(i < 7)
   {
      cherryValue[i] = Number(flash.external.ExternalInterface.call("CherryGfx_Default", i));
      cherryShow(i);
      flash.external.ExternalInterface.call("CherryGfx_Set", i, cherryValue[i]);
      i++;
   }
};

var cherryOrigKeyDown = this.ProcessKeyDown;
this.ProcessKeyDown = function(details, pathToFocus)
{
   var nav = details.navEquivalent;
   if(bPush == false && nav == gfx.ui.NavigationCode.GAMEPAD_X)
   {
      flash.external.ExternalInterface.call("PlayMenuSoundCue", 1);
      flash.external.ExternalInterface.call("CherryGfx_ToggleFps");
      return true;
   }
   if(bPush == false && (nav == gfx.ui.NavigationCode.GAMEPAD_Y || nav == gfx.ui.NavigationCode.GAMEPAD_L1 || nav == gfx.ui.NavigationCode.GAMEPAD_R1))
   {
      flash.external.ExternalInterface.call("PlayMenuSoundCue", 1);
      cherrySetPage(1 - cherryPage);
      return true;
   }
   return cherryOrigKeyDown.call(cherryMc, details, pathToFocus);
};

cherryLoad();
var i = 0;
while(i < 7)
{
   var label = but_sub.duplicateMovieClip("cherry_label" + i, 400 + i);
   label._x = cherryOrigLabels[i]._x;
   label._y = cherryOrigLabels[i]._y;
   label._visible = false;
   cherryLabels[i] = label;
   var value = sub_vol.duplicateMovieClip("cherry_value" + i, 420 + i);
   if(i == 4 || i == 5)
   {
      value._x = aim_vol._x + (sub_vol._x - aim_vol._x) * (i - 3) / 3;
      value._y = aim_vol._y + (sub_vol._y - aim_vol._y) * (i - 3) / 3;
   }
   else
   {
      value._x = cherryOrigValues[i]._x;
      value._y = cherryOrigValues[i]._y;
   }
   value._visible = false;
   cherryValues[i] = value;
   cherryMc["MovFocus_" + cherryFocusNames[i]] = cherryMakeFocus(i);
   cherryMc["MovUnFocus_" + cherryFocusNames[i]] = cherryMakeUnFocus(i);
   i++;
}
cherryWrapOut(0, "Focus_Vibration_Out");
cherryWrapOut(1, "Focus_CameraUpDown_Out");
cherryWrapOut(2, "Focus_CameraRL_Out");
cherryWrapOut(3, "Focus_Aiming_Out");
cherryWrapOut(4, "Focus_BGM_Out_1");
cherryWrapOut(4, "Focus_BGM_Out_2");
cherryWrapOut(5, "Focus_SE_Out_1");
cherryWrapOut(5, "Focus_SE_Out_2");
cherryWrapOut(6, "Focus_Subtitle_Out");

createTextField("cherryHint", 460, def_vol._x, def_vol._y + 48, 10, 10);
cherryHint.html = false;
cherryHint.embedFonts = true;
cherryHint.selectable = false;
cherryHint.autoSize = "left";
cherryHint.setNewTextFormat(new TextFormat("$AshUIFont01", 22, 16777215));
cherrySetText(cherryHint, "Y  GRAPHICS     X  FPS COUNTER", "left");
createEmptyMovieClip("cherryTick", 461).onEnterFrame = cherryRefresh;
