/*
 * MenuItem.cpp
 *
 *  Created on: 7 May 2018
 *      Author: David
 */

#include "MenuItem.h"
#include "RepRap.h"
#include "Heating/Heat.h"
#include "Platform.h"
#include "GCodes/GCodes.h"
#include "Movement/Move.h"
#include "Display.h"

MenuItem::MenuItem(PixelNumber r, PixelNumber c, FontNumber fn)
	: row(r), column(c), fontNumber(fn), next(nullptr)
{
}

/*static*/ void MenuItem::AppendToList(MenuItem **root, MenuItem *item)
{
	while (*root != nullptr)
	{
		root = &((*root)->next);
	}
	item->next = nullptr;
	*root = item;
}

ButtonMenuItem *ButtonMenuItem::freelist = nullptr;

ButtonMenuItem::ButtonMenuItem(PixelNumber r, PixelNumber c, FontNumber fn, const char* t, const char* cmd, char const* acFile)
	: MenuItem(r, c, fn), text(t), command(cmd), m_acFile(acFile)
{
}

void ButtonMenuItem::Draw(Lcd7920& lcd, PixelNumber rightMargin, bool highlight)
{
	lcd.SetCursor(row, column);
	lcd.SetRightMargin(rightMargin);

	lcd.TextInvert(highlight);
	lcd.print(text); // TODO: create Print(char[], bool) to combine these two lines

	lcd.TextInvert(false);
	lcd.ClearToMargin();
}

const char* ButtonMenuItem::Select()
{
	const char *szPtr;

	// If we're "menu", just return the name -- but a problem if the name begins with 'G', 'M' or 'T'
	// If we're "return", send out "return"

	if (0 == strcmp("menu", command))
		szPtr = m_acFile;
	else
		szPtr = command; // includes "return"

	return szPtr;
}

ValueMenuItem::ValueMenuItem(PixelNumber r, PixelNumber c, FontNumber fn, PixelNumber w, unsigned int v, unsigned int d)
	: MenuItem(r, c, fn), valIndex(v), currentValue(0.0), width(w), decimals(d), adjusting(false)
{
}

void ValueMenuItem::Draw(Lcd7920& lcd, PixelNumber rightMargin, bool highlight)
{
	lcd.SetCursor(row, column);
	lcd.SetRightMargin(min<PixelNumber>(column + width, rightMargin));
	lcd.TextInvert(highlight);

	bool error = false;
	if (!adjusting)
	{
		const unsigned int itemNumber = valIndex % 100;
		switch (valIndex/100)
		{
		case 0:		// heater current temperature
			currentValue = reprap.GetGCodes().GetItemCurrentTemperature(itemNumber);
			break;

		case 1:		// heater active temperature
			currentValue = reprap.GetGCodes().GetItemActiveTemperature(itemNumber);
			break;

		case 2:		// heater standby temperature
			currentValue = reprap.GetGCodes().GetItemStandbyTemperature(itemNumber);
			break;

		case 3:		// fan %
			currentValue = ((itemNumber == 99)
							? reprap.GetGCodes().GetMappedFanSpeed()
							: reprap.GetPlatform().GetFanValue(itemNumber)
						   ) * 100.0;
			break;

		case 4:		// extruder %
			currentValue = reprap.GetGCodes().GetExtrusionFactor(itemNumber) * 100.0;
			break;

		case 5:		// misc
			switch (itemNumber)
			{
			case 0:
				currentValue = reprap.GetGCodes().GetSpeedFactor() * 100.0;
				break;

			case 10: // X
				{
					float m[MaxAxes];
					reprap.GetMove().GetCurrentMachinePosition(m, false);
					currentValue = m[X_AXIS];
				}
				break;

			case 11: // Y
				{
					float m[MaxAxes];
					reprap.GetMove().GetCurrentMachinePosition(m, false);
					currentValue = m[Y_AXIS];
				}
				break;

			case 12: // Z
				{
					float m[MaxAxes];
					reprap.GetMove().GetCurrentMachinePosition(m, false);
					currentValue = m[Z_AXIS];
				}
				break;

			case 13: // E0
				currentValue = reprap.GetGCodes().GetRawExtruderTotalByDrive(0);
				break;

			case 14: // E1
				currentValue = reprap.GetGCodes().GetRawExtruderTotalByDrive(1);
				break;

			case 15: // E2
				currentValue = reprap.GetGCodes().GetRawExtruderTotalByDrive(2);
				break;

			case 16: // E3
				currentValue = reprap.GetGCodes().GetRawExtruderTotalByDrive(3);
				break;

			case 20:
				currentValue = reprap.GetCurrentToolNumber();
				break;

			default:
				error = true;
			}
			break;

		default:
			error = true;
			break;
		}
	}

	if (error)
	{
		lcd.print("***");
	}
	else
	{
		lcd.print(currentValue, decimals);
	}
	lcd.ClearToMargin();
}

const char* ValueMenuItem::Select()
{
	adjusting = true;
	return nullptr;
}

bool ValueMenuItem::Adjust_SelectHelper()
{
	const unsigned int itemNumber = valIndex % 100;

	bool error = false;
	switch (valIndex/100)
	{
	case 1:		// heater active temperature
		if (0 == currentValue) // 0 is off, otherwise ensure the tool is made active at the same time
		{
			reprap.GetGCodes().SetItemActiveTemperature(itemNumber, currentValue);
		}
		else
		{
			unsigned int uToolNumber = itemNumber;
			if (79 == itemNumber)
			{
				uToolNumber = reprap.GetCurrentToolNumber();
			}

			reprap.SelectTool(uToolNumber, false);
			reprap.GetGCodes().SetItemActiveTemperature(uToolNumber, currentValue);
		}
		break;

	case 2:		// heater standby temperature
		reprap.GetGCodes().SetItemStandbyTemperature(itemNumber, currentValue);
		break;

	case 3:		// fan %
		if (itemNumber == 99)
		{
			reprap.GetGCodes().SetMappedFanSpeed(currentValue * 0.01);
		}
		else
		{
			reprap.GetPlatform().SetFanValue(itemNumber, currentValue * 0.01);
		}
		break;

	case 4:		// extruder %
		reprap.GetGCodes().SetExtrusionFactor(itemNumber, currentValue * 0.01);
		break;

	case 5:		// misc
		switch (itemNumber)
		{
		case 0:
			reprap.GetGCodes().SetSpeedFactor(currentValue * 0.01);
			break;

		case 20:
			reprap.SelectTool(currentValue, false);
			break;

		default:
			error = true;
			break;
		}
		break;

	default:
		error = true;
		break;
	}

	if (error)
	{
		reprap.GetDisplay().ErrorBeep();
	}
	adjusting = false;

	return true;
}

bool ValueMenuItem::Adjust_AlterHelper(int clicks)
{
	const unsigned int itemNumber = valIndex % 100;

	switch (valIndex/100)
	{
	case 5:
		switch (itemNumber)
		{
		case 0:
			currentValue = constrain<float>(currentValue + (float)clicks, 10, 500);
			break;

		case 20:
			currentValue = constrain<int>(currentValue + clicks, -1, 255);
			break;

		default:
			// error = true;
			break;
		}
		break;

	default:
		currentValue += (float)clicks;			// currently we always adjust by 1
		break;
	}

	return false;
}

bool ValueMenuItem::Adjust(int clicks)
{
	if (clicks == 0)	// if button has been pressed
	{
		return Adjust_SelectHelper();
	}

	// Wheel has scrolled: alter value
	return Adjust_AlterHelper(clicks);
}

FilesMenuItem *FilesMenuItem::freelist = nullptr;

FilesMenuItem::FilesMenuItem(PixelNumber r, PixelNumber c, FontNumber fn, const char *cmd, const char *dir, unsigned int nf, unsigned int uFontHeight)
	: MenuItem(r, c, fn), command(cmd), initialDirectory(dir), m_uDisplayLines(nf), m_uFontHeight(uFontHeight),
        m_uFirstFileVisible(0), m_uCurrentSelectedFile(0), m_oMS(reprap.GetPlatform().GetMassStorage())
{
	m_acCommand[0] = '\0';

	EnterDirectory(initialDirectory);
}

void FilesMenuItem::EnterDirectory(const char *acDir)
{
	m_uTotalFilesInCurrentDirectory = 0;

	// TODO: these lens may need to be +1 for the null character
	SafeStrncpy(m_acCurrentDirectory, acDir, MaxFilenameLength);

	FileInfo oFileInfo;
	if (m_oMS->FindFirst(m_acCurrentDirectory, oFileInfo))
	{
		do
		{
			++m_uTotalFilesInCurrentDirectory;
		}
		while (m_oMS->FindNext(oFileInfo));
	}
}

void FilesMenuItem::Draw(Lcd7920& lcd, PixelNumber rightMargin, bool highlight)
{
	lcd.SetCursor(row, column);
	lcd.SetRightMargin(rightMargin);

	// We are writing text to line numbers 0, 1, 2 ... m_uDisplayLines - 1
	// These are file entries m_nCurrentSelectedFile, m_nCurrentSelectedFile + 1, m_nCurrentSelectedFile + 2 ... m_nCurrentSelectedFile + m_uDisplayLines - 1 within the directory

	// TODO: this must not be run when the SD card is ejected!

	// Seek to the first file that is in view
	int nDirListingLocation = -1;
	FileInfo oFileInfo;
	if (m_oMS->FindFirst(m_acCurrentDirectory, oFileInfo))
	{
		do
		{
			++nDirListingLocation;
		}
		// Must short-circuit in this order!
		while ((nDirListingLocation < static_cast<int>(m_uFirstFileVisible)) && m_oMS->FindNext(oFileInfo));
	}

	// TODO: if not current directory same as initial directory, include item ".." on the list
	//   This also becomes a special case for Select()

	// TODO: allow sorting by filename?  seems to be output in order of file creation

	bool bFileSystemError = false;
	for (uint8_t i = 0; (i < m_uDisplayLines) && !bFileSystemError; ++i, bFileSystemError = !(m_oMS->FindNext(oFileInfo)))
	{
		lcd.SetCursor(row + (m_uFontHeight * i), column);

		// char acFileName[18];

		if (m_uTotalFilesInCurrentDirectory > i + m_uFirstFileVisible)
		{
			// lcd.TextInvert(highlight);
			if (highlight && (m_uCurrentSelectedFile == (i + m_uFirstFileVisible)))
			{
				lcd.print("> ");
			}
			else
			{
				lcd.print("  ");
			}

			if (oFileInfo.isDirectory)
				lcd.print("./");

			lcd.print(oFileInfo.fileName);
			// lcd.print(m_nFirstFileVisible + i);
			// lcd.TextInvert(false);
		}

		lcd.ClearToMargin();
	}

	// TODO: cache these filenames to avoid the SD overhead each time...
}

void FilesMenuItem::Enter(bool bForwardDirection)
{
	if (bForwardDirection)
	{
		m_uCurrentSelectedFile = 0;
		m_uFirstFileVisible = 0;
	}
	else
	{
		// TODO: graceful handling of empty directory
		m_uCurrentSelectedFile = m_uTotalFilesInCurrentDirectory - 1;
		m_uFirstFileVisible = ((m_uTotalFilesInCurrentDirectory > m_uDisplayLines) ? (m_uTotalFilesInCurrentDirectory - m_uDisplayLines) : 0);
	}
}

int FilesMenuItem::Advance(int nCounts)
{
	while (nCounts > 0)
	{
		// Advancing one more would take us past the end of the list
		// Instead, return the remaining count so that the other
		// selectable menu items can be scrolled.
		if (m_uTotalFilesInCurrentDirectory == m_uCurrentSelectedFile + 1)
			break;

		++m_uCurrentSelectedFile;
		--nCounts;

		// Move the visible portion of the list down, if required
		if (m_uCurrentSelectedFile == m_uFirstFileVisible + m_uDisplayLines)
			++m_uFirstFileVisible;
	}

	while (nCounts < 0)
	{
		if (0 == m_uCurrentSelectedFile)
			break;

		--m_uCurrentSelectedFile;
		++nCounts;

		// Move the visible portion of the list up, if required
		if (m_uCurrentSelectedFile < m_uFirstFileVisible)
			--m_uFirstFileVisible;
	}

	return nCounts;
}

const char* FilesMenuItem::Select()
{
	// Several cases:
	// 1. File - run command with filename as argument
	// TODO 2. Directory - call EnterDirectory(), adding to saved state information
	// TODO 3. ".." entry - call EnterDirectory(), using saved state information

	// Get information on the item selected

	// TODO: this must not be allowed when the SD card is ejected!

	// Seek to the first file that is in view
	int nDirectoryLocation = -1;
	FileInfo oFileInfo;
	if (m_oMS->FindFirst(m_acCurrentDirectory, oFileInfo))
	{
		do
		{
			++nDirectoryLocation;
		}
		while ((nDirectoryLocation != static_cast<int>(m_uCurrentSelectedFile)) && m_oMS->FindNext(oFileInfo)); // relying on short-circuit -- don't change order!
	}

	if (oFileInfo.isDirectory)
	{
		return nullptr;
	}
	else
	{
		m_acCommand[0] = '\0';

		SafeStrncpy(m_acCommand, command, strlen(command) + 1);

		int nReplacementIndex = StringContains(m_acCommand, "#0");
		if (-1 != nReplacementIndex)
		{
			nReplacementIndex -= strlen("#0");

			SafeStrncpy(m_acCommand + nReplacementIndex, "\"", 2);
			++nReplacementIndex;

			SafeStrncpy(m_acCommand + nReplacementIndex, oFileInfo.fileName, strlen(oFileInfo.fileName) + 1);
			nReplacementIndex += strlen(oFileInfo.fileName);

			SafeStrncpy(m_acCommand + nReplacementIndex, "\"", 2);
			// ++nReplacementIndex // unused

			// debugPrintf("Attempting: %s\n", m_acCommand);
		}

		return m_acCommand; // would otherwise break encapsulation, but the return is const
	}
}

// End

