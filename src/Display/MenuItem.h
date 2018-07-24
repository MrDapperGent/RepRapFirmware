/*
 * MenuItem.h
 *
 *  Created on: 7 May 2018
 *      Author: David
 */

#ifndef SRC_DISPLAY_MENUITEM_H_
#define SRC_DISPLAY_MENUITEM_H_

#include "Libraries/General/FreelistManager.h"
#include "RepRapFirmware.h"
#include "ST7920/lcd7920.h"
#include "Storage/MassStorage.h"

// Menu item class hierarchy
class MenuItem
{
public:
	typedef uint8_t FontNumber;

	// Draw this element on the LCD respecting 'maxWidth' and 'highlight'
	virtual void Draw(Lcd7920& lcd, PixelNumber maxWidth, bool highlight) = 0;

	// Select this element with a push of the encoder.
	// If it returns nullptr then go into adjustment mode.
	// Else execute the returned command.
	virtual const char* Select() = 0;

	// Actions to be taken when the menu system selects this item
	virtual void Enter(bool bForwardDirection) {};

	// Actions to be taken when the menu system receives encoder counts
	// and this item is currently selected
	// TODO: may be able to merge down with Adjust()
	virtual int Advance(int nCounts) { return nCounts; }

	// Adjust this element, returning true if we have finished adjustment.
	// 'clicks' is the number of encoder clicks to adjust by, or 0 if the button was pushed.
	virtual bool Adjust(int clicks) { return true; }

	virtual ~MenuItem() { }

	MenuItem *GetNext() const { return next; }
	FontNumber GetFontNumber() const { return fontNumber; }

	static void AppendToList(MenuItem **root, MenuItem *item);

protected:
	MenuItem(PixelNumber r, PixelNumber c, FontNumber fn);

	const PixelNumber row, column;
	const FontNumber fontNumber;

private:
	MenuItem *next;
};

class ButtonMenuItem : public MenuItem
{
public:
	void* operator new(size_t sz) { return Allocate<ButtonMenuItem>(); }
	void operator delete(void* p) { Release<ButtonMenuItem>(p); }

	ButtonMenuItem(PixelNumber r, PixelNumber c, FontNumber fn, const char *t, const char *cmd);
	void Draw(Lcd7920& lcd, PixelNumber maxWidth, bool highlight) override;
	const char* Select() override { return command; }

private:
	static ButtonMenuItem *freelist;

	const char *text;
	const char *command;
};

class ValueMenuItem : public MenuItem
{
public:
	void* operator new(size_t sz) { return Allocate<ValueMenuItem>(); }
	void operator delete(void* p) { Release<ValueMenuItem>(p); }

	ValueMenuItem(PixelNumber r, PixelNumber c, FontNumber fn, PixelNumber w, unsigned int v, unsigned int d);
	void Draw(Lcd7920& lcd, PixelNumber maxWidth, bool highlight) override;
	const char* Select() override;
	bool Adjust(int clicks) override;

private:
	unsigned int valIndex;
	float currentValue;
	PixelNumber width;
	uint8_t decimals;
	bool adjusting;
};

class FilesMenuItem : public MenuItem
{
public:
	void* operator new(size_t sz) { return Allocate<FilesMenuItem>(); }
	void operator delete(void* p) { Release<FilesMenuItem>(p); }

	FilesMenuItem(PixelNumber r, PixelNumber c, FontNumber fn, const char *cmd, const char *dir, unsigned int nf, unsigned int uFontHeight);
	void Draw(Lcd7920& lcd, PixelNumber rightMargin, bool highlight) override;
	void Enter(bool bForwardDirection) override;
	int Advance(int nCounts) override;
	const char* Select() override;

	void EnterDirectory(const char *acDir);

private:
	static FilesMenuItem *freelist;

	const char *command;
	const char *initialDirectory;
	char m_acCurrentDirectory[MaxFilenameLength];
	unsigned int m_uDisplayLines;
	unsigned int m_uFontHeight;

	char m_acCommand[MaxFilenameLength + 20]; // TODO fix to proper max length

	unsigned int m_uTotalFilesInCurrentDirectory;
	unsigned int m_uFirstFileVisible;
	unsigned int m_uCurrentSelectedFile;

	MassStorage *const m_oMS;
};

#endif /* SRC_DISPLAY_MENUITEM_H_ */

