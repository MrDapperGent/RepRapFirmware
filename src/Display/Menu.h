/*
 * Menu.h
 *
 *  Created on: 22 Jan 2018
 *      Author: David
 */

#ifndef SRC_DISPLAY_MENU_H_
#define SRC_DISPLAY_MENU_H_

#include "MenuItem.h"

// Class to represent either a full page menu or a popup menu.
// For space reasons we store only a single instance of this class. Each nested menu is indented by a fixed margin from its parent.
class Menu
{
public:
	Menu(Lcd7920& refLcd, const LcdFont * const fnts[], size_t nFonts);
	void Load(const char* filename);							// load a menu file
	void Pop();
	void EncoderAction(int action);
	void Refresh();

private:
	void ResetCache();
	void Reload();
	const char *ParseMenuLine(char *s);
	void LoadError(const char *msg, unsigned int line);
	void AddItem(MenuItem *item, bool isSelectable);
	const char *const AppendString(const char *s);
	void LoadImage(const char *fname);
	MenuItem *FindHighlightedItem() const;

	static const char *SkipWhitespace(const char *s);
	static char *SkipWhitespace(char *s);

	static const size_t CommandBufferSize = 512;
	static const size_t MaxMenuLineLength = 80; // adjusts behavior in Reload()
	static const size_t MaxMenuFilenameLength = 18;
	static const size_t MaxMenuNesting = 5;						// maximum number of nested menus
	static const PixelNumber InnerMargin = 2;					// how many pixels we keep clear inside the border
	static const PixelNumber OuterMargin = 8 + InnerMargin;		// how many pixels of the previous menu we leave on each side
	static const PixelNumber DefaultNumberWidth = 20;			// default numeric field width

	Lcd7920& lcd;
	const LcdFont * const *fonts;
	const size_t numFonts;

	MenuItem *selectableItems;									// selectable items at the innermost level
	MenuItem *unSelectableItems;								// unselectable items at the innermost level
	String<MaxMenuFilenameLength> filenames[MaxMenuNesting];
	size_t numNestedMenus;
	int numSelectableItems;
	int m_nHighlightedItem;
	bool itemIsSelected;

	// Variables used while parsing
	size_t commandBufferIndex;
	unsigned int fontNumber;
	PixelNumber currentMargin;
	PixelNumber row, column;

	// Buffer for commands to be executed when the user presses a selected item
	char commandBuffer[CommandBufferSize];
};

#endif /* SRC_DISPLAY_MENU_H_ */
