/*
 * Menu.cpp
 *
 *  Created on: 22 Jan 2018
 *      Author: David
 *
 *  Menus are read from files in the /menu folder of the SD card. the root menu is called 'main'.
 *  Each menu file holds a sequence of commands, one per line.
 *  The following commands are supported:
 *
 *  image [Rnn] [Cnn] [Fnn] L"filename"
 *    ; display the image from "filename" at position RC
 *  text [Rnn] [Cnn] [Fnn] T"text"
 *    ; display non-selectable "text" at position RC
 *  button [Rnn] [Cnn] [Fnn] T"text" A"action" [L"filename"]
 *    ; display selectable "text" at RC, perform action when clicked
 *  value [Rnn] [Cnn] [Fnn] [Dnn] Wnnn Nvvv
 *    ; display the specified value at RC to the specified number of decimal places in the specified width
 *  alter [Rnn] [Cnn] [Fnn] [Dnn] Wnnn Nvvv
 *    ; display the specified value at RC to the specified number of decimal places in the specified width and allow it to be altered
 *  files [Rnn] [Fnn] Nnn I"initial-directory" A"action" [L"filename"]
 *    ; display a list of files N lines high and allow them to be selected. The list uses the full width of the display.
 *
 *  Rnn is the row number for the top of the element measured in pixels from the top of the display
 *  Cnn is the column number for the left of the element measured in pixels from the left hand edge of the display
 *  Fnn is the font to use, 0=small 1=large
 *  Wnn is the width in pixels for the element
 *
 *  "action" can be any of:
 *  - a Gcode command string (must begin with G, M or T). In such a string, #0 represents the full name of the current file, in double quotes, set when a file is selected
 *  - "menu" (chains to the menu file given in the L parameter)
 *  - "popup" (pops up the menu given in the L parameter)
 *    NOTE: not currently implemented
 *  - "return" (returns to the parent menu)
 *  Multiple actions can be specified, separated by the vertical-bar character, e.g. "M32 #0|return|return|menu" but 'menu' may only be the last command
 *
 *  The N parameter in the "value" and "alter" commands specifies the value to display or change as follows:
 *  000-078		Tool N first heater current temperature e.g. 0 = tool 0 current temperature (display only)
 *  079			Currently selected tool first heater current temperature (display only)
 *  080-089		Bed heater (N-80) current temperature e.g. 80 = bed heater 0 current temperature (display only)
 *  090-099		Chamber heater (N-90) current temperature e.g. 90 = chamber heater 0 current temperature (display only)
 *  100-178		Tool (N-100) first heater active temperature e.g. 100 = tool 0 active temperature
 *  179         Currently selected tool first heater active temperature
 *  180-189		Bed heater (N-180) active temperature e.g. 180 = bed heater 0 active temperature
 *  190-199		Chamber heater (N-190) active temperature e.g. 190 = chamber heater 0 active temperature
 *  200-278		Tool (N-200) first heater standby temperature e.g. 200 = tool 0 standby temperature
 *  279         Currently selected tool first heater standby temperature
 *  280-289		Bed heater (N-280) standby temperature e.g. 280 = bed heater 0 standby temperature
 *  290-299		Chamber heater (N-290) standby temperature e.g. 290 = chamber heater 0 standby temperature
 *  300-398		Fan (N-300) percent full PWM e.g. 302 = fan 2 percent
 *  399			Current tool fan percent full PWM
 *  400-499		Extruder (N-400) extrusion factor
 *  500			Speed factor
 *  510-516		Current axis location (X, Y, Z, E0, E1, E2, E3 respectively) (display only)
 *  519			Z baby-step offset (display only)
 *  520			Currently selected tool number
 */

#include "Menu.h"
#include "ST7920/lcd7920.h"
#include "RepRap.h"
#include "Platform.h"
#include "Storage/MassStorage.h"
#include "GCodes/GCodes.h"
#include "Display/Display.h"
#include "PrintMonitor.h"

Menu::Menu(Lcd7920& refLcd, const LcdFont * const fnts[], size_t nFonts)
	: lcd(refLcd), fonts(fnts), numFonts(nFonts),
	  m_bTimeoutEnabled(false), m_uLastActionTime(millis()),
	  selectableItems(nullptr), unSelectableItems(nullptr), numNestedMenus(0), numSelectableItems(0), m_nHighlightedItem(0), itemIsSelected(false),
	  m_tRowOffset(0)
{
}

void Menu::Load(const char* filename)
{
	if (numNestedMenus < MaxMenuNesting)
	{
		filenames[numNestedMenus].copy(filename);

		m_tRowOffset = 0;

		if (numNestedMenus == 0)
		{
			currentMargin = 0;
			lcd.Clear();
		}
		else
		{
			currentMargin = 0;
			const PixelNumber right = NumCols;
			const PixelNumber bottom = NumRows;
			lcd.Clear(currentMargin, currentMargin, bottom, right);

			// Draw the outline
			// lcd.Line(currentMargin, currentMargin, bottom, currentMargin, PixelMode::PixelSet);
			// lcd.Line(currentMargin, currentMargin, currentMargin, right, PixelMode::PixelSet);
			// lcd.Line(bottom, currentMargin, bottom, right, PixelMode::PixelSet);
			// lcd.Line(currentMargin, right, bottom, right, PixelMode::PixelSet);

			// currentMargin += InnerMargin;
		}

		++numNestedMenus;
		Reload();
	}
}

void Menu::Pop()
{
	// currentMargin = 0;
	lcd.Clear();
	m_tRowOffset = 0;
	--numNestedMenus;
	Reload();
}

void Menu::LoadError(const char *msg, unsigned int line)
{
	// Remove selectable items that may obscure view of the error message
	ResetCache();

	lcd.Clear(currentMargin, currentMargin, NumRows - currentMargin, NumCols - currentMargin);
	lcd.SetFont(fonts[0]);
	lcd.print("Error loading menu\nFile ");
	lcd.print(filenames[numNestedMenus - 1].c_str());
	if (line != 0)
	{
		lcd.print("\nLine ");
		lcd.print(line);
	}
	lcd.write('\n');
	lcd.print(msg);

	if (numNestedMenus > 1)
	{
		// TODO add control to pop previous menu here, or revert to main menu after some time
	}
}

// Parse a line in a menu layout file returning any error message, or nullptr if there was no error.
// Leading whitespace has already been skipped.
const char *Menu::ParseMenuLine(char *commandWord)
{
	// Check for blank or comment line
	if (*commandWord == ';' || *commandWord == 0)
	{
		return nullptr;
	}

	// Find the first word
	char *args = commandWord;
	while (isalpha(*args))
	{
		++args;
	}
	if (args == commandWord || (*args != ' ' && *args != '\t' && *args != 0))
	{
		return "Bad command";
	}

	if (*args != 0)
	{
		*args = 0;		// null terminate command word
		++args;
	}

	// Parse the arguments
	unsigned int decimals = 0;
	unsigned int nparam = 0;
	unsigned int width = DefaultNumberWidth;
	const char *text = "*";
	const char *fname = "main";
	const char *dirpath = "";
	const char *action = nullptr;

	while (*args != 0 && *args != ';')
	{
		char ch;
		switch (ch = toupper(*args++))
		{
		case ' ':
		case '\t':
			break;

		case 'R':
			row = strtoul(args, &args, 10u);
			break;

		case 'C':
			column = strtoul(args, &args, 10u);
			break;

		case 'F':
			fontNumber = min<unsigned int>(strtoul(args, &args, 10u), numFonts - 1);
			break;

		case 'D':
			decimals = strtoul(args, &args, 10u);
			break;

		case 'N':
			nparam = strtoul(args, &args, 10u);
			break;

		case 'W':
			width = strtoul(args, &args, 10u);
			break;

		case 'T':
		case 'L':
		case 'A':
		case 'I':
			if (*args != '"')
			{
				return "Missing string arg";
			}
			++args;
			((ch == 'T') ? text : (ch == 'A') ? action : (ch == 'I') ? dirpath : fname) = args;
			while (*args != '"' && *args != 0)
			{
				++args;
			}
			if (*args == '"')
			{
				*args = 0;
				++args;
			}
			break;

		default:
			return "Bad arg letter";
		}
	}

	lcd.SetCursor(row + currentMargin, column + currentMargin);

	// Create an object resident in memory corresponding to the menu layout file's description
	if (StringEquals(commandWord, "text"))
	{
		const char *const acText = AppendString(text);
		AddItem(new TextMenuItem(row, column, fontNumber, acText), false);

		lcd.SetFont(fonts[fontNumber]);
		lcd.print(text);
		row = lcd.GetRow() - currentMargin;
		column = lcd.GetColumn() - currentMargin;
	}
	else if (StringEquals(commandWord, "image") && fname != nullptr)
	{
		LoadImage(fname);
	}
	else if (StringEquals(commandWord, "button"))
	{
		if (ShowBasedOnPrinterState(text, fname))
		{
			const char * const textString = AppendString(text);
			const char * const actionString = AppendString(action);
			const char *const c_acFileString = AppendString(fname);
			AddItem(new ButtonMenuItem(row, column, fontNumber, textString, actionString, c_acFileString), true);
			// Print the button as well so that we can update the row and column
			lcd.SetFont(fonts[fontNumber]);
			lcd.print(text);
			row = lcd.GetRow() - currentMargin;
			column = lcd.GetColumn() - currentMargin;
		}
	}
	else if (StringEquals(commandWord, "value"))
	{
		AddItem(new ValueMenuItem(row, column, fontNumber, width, nparam, decimals), false);
		column += width;
	}
	else if (StringEquals(commandWord, "alter"))
	{
		AddItem(new ValueMenuItem(row, column, fontNumber, width, nparam, decimals), true);
		column += width;
	}
	else if (StringEquals(commandWord, "files"))
	{
		const char * const actionString = AppendString(action);
		const char *const dir = AppendString(dirpath);
		const char *const acFileString = AppendString(fname);
		AddItem(new FilesMenuItem(row, column, fontNumber, actionString, dir, acFileString, nparam, fonts[fontNumber]->height), true);
		//TODO update row by a sensible value e.g. nparam * text row height
		column = 0;
	}
	else
	{
		return "Unknown command";
	}

	return nullptr;
}

void Menu::ResetCache()
{
	// Delete the existing items
	while (selectableItems != nullptr)
	{
		MenuItem *current = selectableItems;
		selectableItems = selectableItems->GetNext();
		delete current;
	}
	while (unSelectableItems != nullptr)
	{
		MenuItem *current = unSelectableItems;
		unSelectableItems = unSelectableItems->GetNext();
		delete current;
	}
	numSelectableItems = 0;
	m_nHighlightedItem = 0;

	return;
}

void Menu::Reload()
{
	ResetCache();

	lcd.SetRightMargin(NumCols - currentMargin);
	const char * const fname = filenames[numNestedMenus - 1].c_str();
	FileStore * const file = reprap.GetPlatform().OpenFile(MENU_DIR, fname, OpenMode::read);
	if (file == nullptr)
	{
		LoadError("Can't open menu file", 0);
	}
	else
	{
#if 0
		lcd.print("Menu");
		lcd.SetCursor(currentMargin + lcd.GetFontHeight() + 1, currentMargin);
		lcd.print(fname);
#else
		row = 0;
		column = 0;
		fontNumber = 0;
		commandBufferIndex = 0; // Free the string buffer, which contains layout elements from an old menu
		for (unsigned int line = 1; ; ++line)
		{
			char buffer[MaxMenuLineLength];
			if (file->ReadLine(buffer, sizeof(buffer)) <= 0)
			{
				break;
			}

			char * const pcMenuLine = SkipWhitespace(buffer);
			const char * const errMsg = ParseMenuLine(pcMenuLine);
			if (errMsg != nullptr)
			{
				LoadError(errMsg, line);
				break;
			}

			// Check for string buffer full
			if (commandBufferIndex == sizeof(commandBuffer))
			{
				LoadError("|Menu buffer full", line);
				break;
			}
		}
#endif
		file->Close();
		// Refresh();
	}
}

void Menu::AddItem(MenuItem *item, bool isSelectable)
{
	MenuItem::AppendToList((isSelectable) ? &selectableItems : &unSelectableItems, item);
	if (isSelectable)
	{
		++numSelectableItems;
	}
}

// Append a string to the string buffer and return its index
const char *const Menu::AppendString(const char *s)
{
	// TODO: hold a fixed reference to '\0' -- if any strings passed in are empty, return this reference
	const size_t oldIndex = commandBufferIndex;
	if (commandBufferIndex < sizeof(commandBuffer))
	{
		SafeStrncpy(commandBuffer + commandBufferIndex, s, sizeof(commandBuffer) - commandBufferIndex);
		commandBufferIndex += strlen(commandBuffer + commandBufferIndex) + 1;
	}
	return commandBuffer + oldIndex;
}

// TODO: there is no error handling if a command within a sequence cannot be accepted...
void Menu::EncoderAction_ExecuteHelper(const char *const cmd)
{
	if (cmd[0] == 'G' || cmd[0] == 'M' || cmd[0] == 'T')
	{
		const bool success = reprap.GetGCodes().ProcessCommandFromLcd(cmd);
		if (success)
		{
			// reprap.GetDisplay().SuccessBeep();
		}
		else
		{
			reprap.GetDisplay().ErrorBeep();			// long low beep
		}
	}
	else
	{
		// "menu" returns the filename (e.g. "main")
		// "return" returns the command itself ("return")
		if (0 == strcmp("return", cmd))
			Pop(); // up one level
		else
			Load(cmd);
	}
}

void Menu::EncoderAction_EnterItemHelper()
{
	MenuItem *const item = FindHighlightedItem();
	if (item != nullptr)
	{
		const char *const cmd = item->Select();
		if (cmd != nullptr)
		{
			char acCurrentCommand[MaxFilenameLength + 20];
			SafeStrncpy(acCurrentCommand, cmd, strlen(cmd) + 1);

			char *pcCurrentCommand = acCurrentCommand;

			int nNextCommandIndex = StringContains(pcCurrentCommand, "|");
			while (-1 != nNextCommandIndex)
			{
				*(pcCurrentCommand + nNextCommandIndex - 1) = '\0';

				EncoderAction_ExecuteHelper(pcCurrentCommand);

				pcCurrentCommand += nNextCommandIndex;

				nNextCommandIndex = StringContains(pcCurrentCommand, "|");
			}
			EncoderAction_ExecuteHelper(pcCurrentCommand);
		}
		else if (item->CanAdjust())
		{
			itemIsSelected = true;
		}
	}
}

void Menu::EncoderAction_AdjustItemHelper(int action)
{
	// Based mainly on file listing requiring we handle list of unknown length
	// before moving on to the next selectable item at the Menu level, we let the
	// currently selected MenuItem try to handle the scroll action itself.  It will
	// return the remainder of the scrolling that it was unable to accommodate.

	MenuItem * const oStartItem = FindHighlightedItem();

	// Let the current menu item attempt to handle scroll wheel first
	action = oStartItem->Advance(action);

	if (0 != action)
	{
		// Otherwise we move through the remaining selectable menu items
		m_nHighlightedItem += action;
		while (m_nHighlightedItem < 0)
		{
			m_nHighlightedItem += numSelectableItems;
		}
		while (m_nHighlightedItem >= numSelectableItems)
		{
			m_nHighlightedItem -= numSelectableItems;
		}

		// Let the newly selected MenuItem handle any selection setup
		MenuItem *const oNewItem = FindHighlightedItem();
		oNewItem->Enter(action > 0);

		PixelNumber tLastOffset = m_tRowOffset;
		m_tRowOffset = oNewItem->GetVisibilityRowOffset(tLastOffset, fonts[oNewItem->GetFontNumber()]);

		if (m_tRowOffset != tLastOffset)
		{
			lcd.Clear();
		}
	}
}

void Menu::EncoderAction_ExitItemHelper(int action)
{
	MenuItem * const item = FindHighlightedItem();
	if (item != nullptr)
	{
		const bool done = item->Adjust(action);
		if (done)
		{
			itemIsSelected = false;
		}
	}
	else
	{
		// Should not get here
		itemIsSelected = false;
	}
}

// Perform the specified encoder action
// If 'action' is zero then the button was pressed, else 'action' is the number of clicks (+ve for clockwise)
// EncoderAction is what's called in response to all wheel/button actions; a convenient place to set new timeout values
void Menu::EncoderAction(int action)
{
	if (numSelectableItems != 0)
	{
		if (itemIsSelected) // send the wheel action (scroll or click) to the item itself
			EncoderAction_ExitItemHelper(action);
		else if (action != 0) // scroll without an item under selection
			EncoderAction_AdjustItemHelper(action);
		else // click without an item under selection
			EncoderAction_EnterItemHelper();
	}

	m_bTimeoutEnabled = true;
	m_uLastActionTime = millis();
}

/*static*/ const char *Menu::SkipWhitespace(const char *s)
{
	while (*s == ' ' || *s == '\t')
	{
		++s;
	}
	return s;
}

/*static*/ char *Menu::SkipWhitespace(char *s)
{
	while (*s == ' ' || *s == '\t')
	{
		++s;
	}
	return s;
}

void Menu::LoadImage(const char *fname)
{
	//TODO
	lcd.print("<image>");
}

// Refresh is called every Spin() of the Display under most circumstances; an appropriate place to check if timeout action needs to be taken
void Menu::Refresh()
{
	if (m_bTimeoutEnabled && (millis() - m_uLastActionTime > 20000)) // 20 seconds following latest user action
	{
		// Go to the top menu (just discard information)
		numNestedMenus = 0;
		Load("main");

		m_bTimeoutEnabled = false;
		// m_uLastActionTime = millis();
	}
	else
	{
		const PixelNumber rightMargin = NumCols - currentMargin;
		int nItemBeingDrawnIndex = 0;

		for (MenuItem *item = selectableItems; item != nullptr; item = item->GetNext())
		{
			lcd.SetFont(fonts[item->GetFontNumber()]);
			item->Draw(lcd, rightMargin, (nItemBeingDrawnIndex == m_nHighlightedItem), m_tRowOffset);
			++nItemBeingDrawnIndex;
		}

		for (MenuItem *item = unSelectableItems; item != nullptr; item = item->GetNext())
		{
			lcd.SetFont(fonts[item->GetFontNumber()]);
			item->Draw(lcd, rightMargin, false, m_tRowOffset);
			// ++nItemBeingDrawnIndex; // unused
		}
	}
}

MenuItem *Menu::FindHighlightedItem() const
{
	MenuItem *p = selectableItems;
	for (int n = m_nHighlightedItem; n > 0 && p != nullptr; --n)
	{
		p = p->GetNext();
	}
	return p;
}

// FUTURE: would like this to become part of the menu file schema, a fixed set
// of status checks each item could use to determine its visibility
bool Menu::ShowBasedOnPrinterState(const char *const acText, const char *const acDescription)
{
	bool bShow = true;

	if (0 == strcmp("s_prepare", acDescription))
	{
		bShow = !reprap.GetGCodes().IsReallyPrinting();
	}
	else if (0 == strcmp("s_tune", acDescription))
	{
		// TODO: what about paused state?  is that Prepare or Tune?
		bShow = reprap.GetGCodes().IsReallyPrinting();
	}
	else if (0 == strcmp("Print from SD »", acText))
	{
		bShow = !reprap.GetPrintMonitor().IsPrinting();
	}
	else if (0 == strcmp("Resume Print", acText))
	{
		bShow = reprap.GetGCodes().IsPaused() || reprap.GetGCodes().IsPausing();
	}
	else if (0 == strcmp("Pause Print", acText))
	{
		bShow = reprap.GetGCodes().IsReallyPrinting() || reprap.GetGCodes().IsResuming();
	}
	else if (0 == strcmp("Mount SD", acText))
	{
		bShow = !reprap.GetPlatform().GetMassStorage()->IsDriveMounted(0);
	}
	else if (0 == strcmp("Unmount SD", acText))
	{
		bShow = reprap.GetPlatform().GetMassStorage()->IsDriveMounted(0);
	}
	else if (0 == strcmp("Cancel Print", acText))
	{
		bShow = reprap.GetPrintMonitor().IsPrinting();
	}

	return bShow;
}

// End

