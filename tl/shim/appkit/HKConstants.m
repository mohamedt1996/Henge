/* Henge's AppKit, the C constants and functions a program imports from AppKit (as opposed to its classes). String
   constants carry the values AppKit's own have (read from the real AppKit), since programs and the system compare
   them by value: attribute names, UTIs, AX strings... */
#import "HKShim.h"

/* ---- Accessibility: attributes, roles, actions, notifications and keys, with the AX strings AppKit gives them ---- */
NSString *const NSAccessibilityAnnouncementKey = @"AXAnnouncementKey";
NSString *const NSAccessibilityAnnouncementRequestedNotification = @"AXAnnouncementRequested";
NSString *const NSAccessibilityApplicationActivatedNotification = @"AXApplicationActivated";
NSString *const NSAccessibilityApplicationDeactivatedNotification = @"AXApplicationDeactivated";
NSString *const NSAccessibilityApplicationHiddenNotification = @"AXApplicationHidden";
NSString *const NSAccessibilityApplicationShownNotification = @"AXApplicationShown";
NSString *const NSAccessibilityAscendingSortDirectionValue = @"AXAscendingSortDirection";
NSString *const NSAccessibilityAttributedStringForRangeParameterizedAttribute = @"AXAttributedStringForRange";
NSString *const NSAccessibilityBackgroundColorTextAttribute = @"AXBackgroundColor";
NSString *const NSAccessibilityBoundsForRangeParameterizedAttribute = @"AXBoundsForRange";
NSString *const NSAccessibilityButtonRole = @"AXButton";
NSString *const NSAccessibilityCancelAction = @"AXCancel";
NSString *const NSAccessibilityCellForColumnAndRowParameterizedAttribute = @"AXCellForColumnAndRow";
NSString *const NSAccessibilityCellRole = @"AXCell";
NSString *const NSAccessibilityCheckBoxRole = @"AXCheckBox";
NSString *const NSAccessibilityChildrenAttribute = @"AXChildren";
NSString *const NSAccessibilityChildrenInNavigationOrderAttribute = @"AXChildrenInNavigationOrder";
NSString *const NSAccessibilityColorWellRole = @"AXColorWell";
NSString *const NSAccessibilityColumnCountAttribute = @"AXColumnCount";
NSString *const NSAccessibilityColumnHeaderUIElementsAttribute = @"AXColumnHeaderUIElements";
NSString *const NSAccessibilityColumnIndexRangeAttribute = @"AXColumnIndexRange";
NSString *const NSAccessibilityColumnRole = @"AXColumn";
NSString *const NSAccessibilityColumnsAttribute = @"AXColumns";
NSString *const NSAccessibilityComboBoxRole = @"AXComboBox";
NSString *const NSAccessibilityConfirmAction = @"AXConfirm";
NSString *const NSAccessibilityContentListSubrole = @"AXContentList";
NSString *const NSAccessibilityContentsAttribute = @"AXContents";
NSString *const NSAccessibilityCreatedNotification = @"AXCreated";
NSString *const NSAccessibilityDecrementAction = @"AXDecrement";
NSString *const NSAccessibilityDefinitionListSubrole = @"AXDefinitionList";
NSString *const NSAccessibilityDescendingSortDirectionValue = @"AXDescendingSortDirection";
NSString *const NSAccessibilityDescriptionAttribute = @"AXDescription";
NSString *const NSAccessibilityDisclosedByRowAttribute = @"AXDisclosedByRow";
NSString *const NSAccessibilityDisclosedRowsAttribute = @"AXDisclosedRows";
NSString *const NSAccessibilityDisclosingAttribute = @"AXDisclosing";
NSString *const NSAccessibilityDisclosureLevelAttribute = @"AXDisclosureLevel";
NSString *const NSAccessibilityDisclosureTriangleRole = @"AXDisclosureTriangle";
NSString *const NSAccessibilityDrawerCreatedNotification = @"AXDrawerCreated";
NSString *const NSAccessibilityEnabledAttribute = @"AXEnabled";
NSString *const NSAccessibilityExpandedAttribute = @"AXExpanded";
NSString *const NSAccessibilityFocusedAttribute = @"AXFocused";
NSString *const NSAccessibilityFocusedUIElementChangedNotification = @"AXFocusedUIElementChanged";
NSString *const NSAccessibilityFocusedWindowChangedNotification = @"AXFocusedWindowChanged";
NSString *const NSAccessibilityFontSizeKey = @"AXFontSize";
NSString *const NSAccessibilityFontTextAttribute = @"AXFont";
NSString *const NSAccessibilityForegroundColorTextAttribute = @"AXForegroundColor";
NSString *const NSAccessibilityGridRole = @"AXGrid";
NSString *const NSAccessibilityGroupRole = @"AXGroup";
NSString *const NSAccessibilityHeaderAttribute = @"AXHeader";
NSString *const NSAccessibilityHelpAttribute = @"AXHelp";
NSString *const NSAccessibilityHelpTagCreatedNotification = @"AXHelpTagCreated";
NSString *const NSAccessibilityHelpTagRole = @"AXHelpTag";
NSString *const NSAccessibilityHorizontalOrientationValue = @"AXHorizontalOrientation";
NSString *const NSAccessibilityHorizontalScrollBarAttribute = @"AXHorizontalScrollBar";
NSString *const NSAccessibilityIdentifierAttribute = @"AXIdentifier";
NSString *const NSAccessibilityImageRole = @"AXImage";
NSString *const NSAccessibilityIncrementAction = @"AXIncrement";
NSString *const NSAccessibilityIncrementorRole = @"AXIncrementor";
NSString *const NSAccessibilityIndexAttribute = @"AXIndex";
NSString *const NSAccessibilityInsertionPointLineNumberAttribute = @"AXInsertionPointLineNumber";
NSString *const NSAccessibilityLayoutChangedNotification = @"AXLayoutChanged";
NSString *const NSAccessibilityLevelIndicatorRole = @"AXLevelIndicator";
NSString *const NSAccessibilityLineForIndexParameterizedAttribute = @"AXLineForIndex";
NSString *const NSAccessibilityLinkedUIElementsAttribute = @"AXLinkedUIElements";
NSString *const NSAccessibilityLinkRole = @"AXLink";
NSString *const NSAccessibilityListRole = @"AXList";
NSString *const NSAccessibilityMainWindowChangedNotification = @"AXMainWindowChanged";
NSString *const NSAccessibilityMarkedMisspelledTextAttribute = @"AXMarkedMisspelled";
NSString *const NSAccessibilityMaxValueAttribute = @"AXMaxValue";
NSString *const NSAccessibilityMenuBarRole = @"AXMenuBar";
NSString *const NSAccessibilityMenuItemRole = @"AXMenuItem";
NSString *const NSAccessibilityMenuRole = @"AXMenu";
NSString *const NSAccessibilityMinValueAttribute = @"AXMinValue";
NSString *const NSAccessibilityMovedNotification = @"AXMoved";
NSString *const NSAccessibilityNumberOfCharactersAttribute = @"AXNumberOfCharacters";
NSString *const NSAccessibilityOrientationAttribute = @"AXOrientation";
NSString *const NSAccessibilityOutlineRole = @"AXOutline";
NSString *const NSAccessibilityOutlineRowSubrole = @"AXOutlineRow";
NSString *const NSAccessibilityParentAttribute = @"AXParent";
NSString *const NSAccessibilityPlaceholderValueAttribute = @"AXPlaceholderValue";
NSString *const NSAccessibilityPopUpButtonRole = @"AXPopUpButton";
NSString *const NSAccessibilityPositionAttribute = @"AXPosition";
NSString *const NSAccessibilityPressAction = @"AXPress";
NSString *const NSAccessibilityPriorityKey = @"AXPriorityKey";
NSString *const NSAccessibilityProgressIndicatorRole = @"AXProgressIndicator";
NSString *const NSAccessibilityRadioButtonRole = @"AXRadioButton";
NSString *const NSAccessibilityRadioGroupRole = @"AXRadioGroup";
NSString *const NSAccessibilityRangeForIndexParameterizedAttribute = @"AXRangeForIndex";
NSString *const NSAccessibilityRangeForLineParameterizedAttribute = @"AXRangeForLine";
NSString *const NSAccessibilityRangeForPositionParameterizedAttribute = @"AXRangeForPosition";
NSString *const NSAccessibilityRequiredAttribute = @"AXRequired";
NSString *const NSAccessibilityResizedNotification = @"AXResized";
NSString *const NSAccessibilityRoleAttribute = @"AXRole";
NSString *const NSAccessibilityRoleDescriptionAttribute = @"AXRoleDescription";
NSString *const NSAccessibilityRowCollapsedNotification = @"AXRowCollapsed";
NSString *const NSAccessibilityRowCountAttribute = @"AXRowCount";
NSString *const NSAccessibilityRowCountChangedNotification = @"AXRowCountChanged";
NSString *const NSAccessibilityRowExpandedNotification = @"AXRowExpanded";
NSString *const NSAccessibilityRowHeaderUIElementsAttribute = @"AXRowHeaderUIElements";
NSString *const NSAccessibilityRowIndexRangeAttribute = @"AXRowIndexRange";
NSString *const NSAccessibilityRowRole = @"AXRow";
NSString *const NSAccessibilityRowsAttribute = @"AXRows";
NSString *const NSAccessibilityRTFForRangeParameterizedAttribute = @"AXRTFForRange";
NSString *const NSAccessibilityScrollAreaRole = @"AXScrollArea";
NSString *const NSAccessibilityScrollBarRole = @"AXScrollBar";
NSString *const NSAccessibilitySearchCurrentElementKey = @"AXStartElement";
NSString *const NSAccessibilitySearchDirectionKey = @"AXDirection";
NSString *const NSAccessibilitySearchDirectionNext = @"AXDirectionNext";
NSString *const NSAccessibilitySearchDirectionPrevious = @"AXDirectionPrevious";
NSString *const NSAccessibilitySearchIdentifiersKey = @"AXSearchKey";
NSString *const NSAccessibilitySearchResultsLimitKey = @"AXResultsLimit";
NSString *const NSAccessibilitySearchTextKey = @"AXSearchText";
NSString *const NSAccessibilitySecureTextFieldSubrole = @"AXSecureTextField";
NSString *const NSAccessibilitySelectedAttribute = @"AXSelected";
NSString *const NSAccessibilitySelectedCellsChangedNotification = @"AXSelectedCellsChanged";
NSString *const NSAccessibilitySelectedChildrenAttribute = @"AXSelectedChildren";
NSString *const NSAccessibilitySelectedChildrenChangedNotification = @"AXSelectedChildrenChanged";
NSString *const NSAccessibilitySelectedChildrenMovedNotification = @"AXSelectedChildrenMoved";
NSString *const NSAccessibilitySelectedColumnsChangedNotification = @"AXSelectedColumnsChanged";
NSString *const NSAccessibilitySelectedRowsAttribute = @"AXSelectedRows";
NSString *const NSAccessibilitySelectedRowsChangedNotification = @"AXSelectedRowsChanged";
NSString *const NSAccessibilitySelectedTextAttribute = @"AXSelectedText";
NSString *const NSAccessibilitySelectedTextChangedNotification = @"AXSelectedTextChanged";
NSString *const NSAccessibilitySelectedTextRangeAttribute = @"AXSelectedTextRange";
NSString *const NSAccessibilitySheetCreatedNotification = @"AXSheetCreated";
NSString *const NSAccessibilityShowMenuAction = @"AXShowMenu";
NSString *const NSAccessibilitySizeAttribute = @"AXSize";
NSString *const NSAccessibilitySliderRole = @"AXSlider";
NSString *const NSAccessibilitySortDirectionAttribute = @"AXSortDirection";
NSString *const NSAccessibilitySplitterRole = @"AXSplitter";
NSString *const NSAccessibilitySplittersAttribute = @"AXSplitters";
NSString *const NSAccessibilityStaticTextRole = @"AXStaticText";
NSString *const NSAccessibilityStrikethroughTextAttribute = @"AXStrikethrough";
NSString *const NSAccessibilityStringForRangeParameterizedAttribute = @"AXStringForRange";
NSString *const NSAccessibilityStyleRangeForIndexParameterizedAttribute = @"AXStyleRangeForIndex";
NSString *const NSAccessibilitySubroleAttribute = @"AXSubrole";
NSString *const NSAccessibilityTabButtonSubrole = @"AXTabButton";
NSString *const NSAccessibilityTabGroupRole = @"AXTabGroup";
NSString *const NSAccessibilityTableRole = @"AXTable";
NSString *const NSAccessibilityTabsAttribute = @"AXTabs";
NSString *const NSAccessibilityTextAreaRole = @"AXTextArea";
NSString *const NSAccessibilityTextFieldRole = @"AXTextField";
NSString *const NSAccessibilityTitleAttribute = @"AXTitle";
NSString *const NSAccessibilityTitleChangedNotification = @"AXTitleChanged";
NSString *const NSAccessibilityTitleUIElementAttribute = @"AXTitleUIElement";
NSString *const NSAccessibilityToolbarButtonAttribute = @"AXToolbarButton";
NSString *const NSAccessibilityToolbarRole = @"AXToolbar";
NSString *const NSAccessibilityTopLevelUIElementAttribute = @"AXTopLevelUIElement";
NSString *const NSAccessibilityUIElementDestroyedNotification = @"AXUIElementDestroyed";
NSString *const NSAccessibilityUnderlineTextAttribute = @"AXUnderline";
NSString *const NSAccessibilityUnitsChangedNotification = @"AXUnitsChanged";
NSString *const NSAccessibilityUnknownRole = @"AXUnknown";
NSString *const NSAccessibilityUnknownSortDirectionValue = @"AXUnknownSortDirection";
NSString *const NSAccessibilityURLAttribute = @"AXURL";
NSString *const NSAccessibilityValueAttribute = @"AXValue";
NSString *const NSAccessibilityValueChangedNotification = @"AXValueChanged";
NSString *const NSAccessibilityValueDescriptionAttribute = @"AXValueDescription";
NSString *const NSAccessibilityVerticalOrientationValue = @"AXVerticalOrientation";
NSString *const NSAccessibilityVerticalScrollBarAttribute = @"AXVerticalScrollBar";
NSString *const NSAccessibilityVisibleCellsAttribute = @"AXVisibleCells";
NSString *const NSAccessibilityVisibleCharacterRangeAttribute = @"AXVisibleCharacterRange";
NSString *const NSAccessibilityVisibleChildrenAttribute = @"AXVisibleChildren";
NSString *const NSAccessibilityVisibleColumnsAttribute = @"AXVisibleColumns";
NSString *const NSAccessibilityVisibleRowsAttribute = @"AXVisibleRows";
NSString *const NSAccessibilityWindowAttribute = @"AXWindow";
NSString *const NSAccessibilityWindowCreatedNotification = @"AXWindowCreated";
NSString *const NSAccessibilityWindowDeminiaturizedNotification = @"AXWindowDeminiaturized";
NSString *const NSAccessibilityWindowMiniaturizedNotification = @"AXWindowMiniaturized";
NSString *const NSAccessibilityWindowMovedNotification = @"AXWindowMoved";
NSString *const NSAccessibilityWindowResizedNotification = @"AXWindowResized";
NSString *const NSAccessibilityZoomButtonAttribute = @"AXZoomButton";

/* ---- Notifications, named as themselves ---- */
NSString *const NSApplicationDidBecomeActiveNotification = @"NSApplicationDidBecomeActiveNotification";
NSString *const NSApplicationDidChangeScreenParametersNotification = @"NSApplicationDidChangeScreenParametersNotification";
NSString *const NSApplicationDidFinishLaunchingNotification = @"NSApplicationDidFinishLaunchingNotification";
NSString *const NSApplicationDidHideNotification = @"NSApplicationDidHideNotification";
NSString *const NSApplicationDidResignActiveNotification = @"NSApplicationDidResignActiveNotification";
NSString *const NSApplicationWillBecomeActiveNotification = @"NSApplicationWillBecomeActiveNotification";
NSString *const NSApplicationWillTerminateNotification = @"NSApplicationWillTerminateNotification";
NSString *const NSMenuDidBeginTrackingNotification = @"NSMenuDidBeginTrackingNotification";
NSString *const NSMenuDidEndTrackingNotification = @"NSMenuDidEndTrackingNotification";
NSString *const NSPreferredScrollerStyleDidChangeNotification = @"NSPreferredScrollerStyleDidChangeNotification";
NSString *const NSScreenColorSpaceDidChangeNotification = @"NSScreenColorSpaceDidChangeNotification";
NSString *const NSSystemColorsDidChangeNotification = @"NSSystemColorsDidChangeNotification";
NSString *const NSTextInputContextKeyboardSelectionDidChangeNotification = @"NSTextInputContextKeyboardSelectionDidChangeNotification";
NSString *const NSViewFrameDidChangeNotification = @"NSViewFrameDidChangeNotification";
NSString *const NSWindowDidBecomeKeyNotification = @"NSWindowDidBecomeKeyNotification";
NSString *const NSWindowDidBecomeMainNotification = @"NSWindowDidBecomeMainNotification";
NSString *const NSWindowDidChangeBackingPropertiesNotification = @"NSWindowDidChangeBackingPropertiesNotification";
NSString *const NSWindowDidChangeOcclusionStateNotification = @"NSWindowDidChangeOcclusionStateNotification";
NSString *const NSWindowDidChangeScreenNotification = @"NSWindowDidChangeScreenNotification";
NSString *const NSWindowDidEndLiveResizeNotification = @"NSWindowDidEndLiveResizeNotification";
NSString *const NSWindowDidEnterFullScreenNotification = @"NSWindowDidEnterFullScreenNotification";
NSString *const NSWindowDidExitFullScreenNotification = @"NSWindowDidExitFullScreenNotification";
NSString *const NSWindowDidMoveNotification = @"NSWindowDidMoveNotification";
NSString *const NSWindowDidResignKeyNotification = @"NSWindowDidResignKeyNotification";
NSString *const NSWindowDidResignMainNotification = @"NSWindowDidResignMainNotification";
NSString *const NSWindowDidResizeNotification = @"NSWindowDidResizeNotification";
NSString *const NSWindowWillCloseNotification = @"NSWindowWillCloseNotification";
NSString *const NSWindowWillEnterFullScreenNotification = @"NSWindowWillEnterFullScreenNotification";
NSString *const NSWindowWillExitFullScreenNotification = @"NSWindowWillExitFullScreenNotification";
NSString *const NSWindowWillMoveNotification = @"NSWindowWillMoveNotification";
NSString *const NSWindowWillStartLiveResizeNotification = @"NSWindowWillStartLiveResizeNotification";
NSString *const NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification = @"NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification";
NSString *const NSWorkspaceActiveSpaceDidChangeNotification = @"NSWorkspaceActiveSpaceDidChangeNotification";
NSString *const NSWorkspaceDidActivateApplicationNotification = @"NSWorkspaceDidActivateApplicationNotification";
NSString *const NSWorkspaceScreensDidSleepNotification = @"NSWorkspaceScreensDidSleepNotification";
NSString *const NSWorkspaceScreensDidWakeNotification = @"NSWorkspaceScreensDidWakeNotification";
NSString *const NSWorkspaceWillPowerOffNotification = @"NSWorkspaceWillPowerOffNotification";

/* ---- Attributed string keys and document attributes ---- */
NSString *const NSBackgroundColorAttributeName = @"NSBackgroundColor";
NSString *const NSDocumentTypeDocumentAttribute = @"DocumentType";
NSString *const NSFontAttributeName = @"NSFont";
NSString *const NSForegroundColorAttributeName = @"NSColor";
NSString *const NSHTMLTextDocumentType = @"NSHTML";
NSString *const NSMarkedClauseSegmentAttributeName = @"NSMarkedClauseSegment";
NSString *const NSParagraphStyleAttributeName = @"NSParagraphStyle";
NSString *const NSShadowAttributeName = @"NSShadow";
NSString *const NSStrikethroughStyleAttributeName = @"NSStrikethrough";
NSString *const NSTextInputReplacementRangeAttributeName = @"NSTextInputReplacementRangeAttributeName";
NSString *const NSUnderlineColorAttributeName = @"NSUnderlineColor";
NSString *const NSUnderlineStyleAttributeName = @"NSUnderline";
NSString *const NSWritingDirectionAttributeName = @"NSWritingDirection";

/* ---- Pasteboards: names and UTI types ---- */
NSString *const NSPasteboardNameDrag = @"Apple CFPasteboard drag";
NSString *const NSPasteboardNameFind = @"Apple CFPasteboard find";
NSString *const NSPasteboardNameGeneral = @"Apple CFPasteboard general";
NSString *const NSPasteboardTypeFileURL = @"public.file-url";
NSString *const NSPasteboardTypeHTML = @"public.html";
NSString *const NSPasteboardTypePNG = @"public.png";
NSString *const NSPasteboardTypeRTF = @"public.rtf";
NSString *const NSPasteboardTypeString = @"public.utf8-plain-text";
NSString *const NSPasteboardTypeTIFF = @"public.tiff";
NSString *const NSPasteboardTypeURL = @"public.url";
NSString *const NSPasteboardTypePDF = @"com.adobe.pdf";
NSString *const NSPasteboardTypeRTFD = @"com.apple.flat-rtfd";
NSString *const NSPasteboardTypeFont = @"com.apple.cocoa.pasteboard.character-formatting";
NSString *const NSPasteboardTypeRuler = @"com.apple.cocoa.pasteboard.paragraph-formatting";
NSString *const NSPasteboardTypeTabularText = @"public.utf8-tab-separated-values-text";
NSString *const NSPasteboardTypeMultipleTextSelection = @"com.apple.cocoa.pasteboard.multiple-text-selection";
NSString *const NSPasteboardTypeSound = @"com.apple.cocoa.pasteboard.sound";
NSString *const NSPasteboardTypeTextFinderOptions = @"com.apple.cocoa.pasteboard.find-panel-search-options";
NSString *const NSFileContentsPboardType = @"NSFileContentsPboardType";

/* ---- The rest ---- */
NSString *const NSAppearanceNameAccessibilityHighContrastAqua = @"NSAppearanceNameAccessibilityAqua";
NSString *const NSAppearanceNameAccessibilityHighContrastDarkAqua = @"NSAppearanceNameAccessibilityDarkAqua";
NSString *const NSAppearanceNameAqua = @"NSAppearanceNameAqua";
NSString *const NSAppearanceNameDarkAqua = @"NSAppearanceNameDarkAqua";
NSString *const NSApplicationLaunchIsDefaultLaunchKey = @"NSApplicationLaunchIsDefaultLaunchKey";
NSString *const NSApplicationLaunchUserNotificationKey = @"NSApplicationLaunchUserNotificationKey";
NSString *const NSCalibratedRGBColorSpace = @"NSCalibratedRGBColorSpace";
NSString *const NSDeviceRGBColorSpace = @"NSDeviceRGBColorSpace";
NSString *const NSEventTrackingRunLoopMode = @"NSEventTrackingRunLoopMode";
NSString *const NSFontNameAttribute = @"NSFontNameAttribute";
NSString *const NSFontSymbolicTrait = @"NSCTFontSymbolicTrait";
NSString *const NSFontTraitsAttribute = @"NSCTFontTraitsAttribute";
NSString *const NSImageCompressionFactor = @"NSImageCompressionFactor";
NSString *const NSImageHintCTM = @"NSImageHintCTM";
NSString *const NSImageNameApplicationIcon = @"NSApplicationIcon";
NSString *const NSImageNameFolder = @"NSFolder";
NSString *const NSPrintAllPages = @"NSPrintAllPages";
NSString *const NSPrintFirstPage = @"NSFirstPage";
NSString *const NSPrintLastPage = @"NSLastPage";
NSString *const NSSharingServiceNameAddToSafariReadingList = @"com.apple.share.System.add-to-safari-reading-list";
NSString *const NSSharingServiceNameComposeEmail = @"com.apple.share.Mail.compose";
NSString *const NSTouchBarItemIdentifierCandidateList = @"NSTouchBarItemIdentifierCandidateList";
NSString *const NSTouchBarItemIdentifierCharacterPicker = @"NSTouchBarItemIdentifierCharacterPicker";
NSString *const NSTouchBarItemIdentifierFlexibleSpace = @"NSTouchBarItemIdentifierFlexibleSpace";
/* ---- font weights ---- */
const CGFloat NSFontWeightUltraLight = -0.8;
const CGFloat NSFontWeightThin = -0.6;
const CGFloat NSFontWeightLight = -0.4;
const CGFloat NSFontWeightRegular = 0;
const CGFloat NSFontWeightMedium = 0.23;
const CGFloat NSFontWeightSemibold = 0.3;
const CGFloat NSFontWeightBold = 0.4;
const CGFloat NSFontWeightHeavy = 0.56;
const CGFloat NSFontWeightBlack = 0.62;

/* ---- functions ---- */

/* As AppKit's: fills with the current fill color, copying (not blending) over what is there. */
void NSRectFill(NSRect rect) {
    CGContextRef c = [NSGraphicsContext currentContext].CGContext;
    if (!c) return;
    CGContextSaveGState(c);
    CGContextSetBlendMode(c, kCGBlendModeCopy);
    CGContextFillRect(c, rect);
    CGContextRestoreGState(c);
}

void NSBeep(void) {}

BOOL NSPerformService(NSString *itemName, NSPasteboard *pboard) { return NO; }

void NSAccessibilityPostNotification(id element, NSString *notification) {}
void NSAccessibilityPostNotificationWithUserInfo(id element, NSString *notification, NSDictionary *userInfo) {}
id NSAccessibilityUnignoredAncestor(id element) { return element; }
NSArray *NSAccessibilityUnignoredChildren(NSArray *children) { return children; }

/* "AXPopUpButton" -> "pop up button", as a stand-in for AppKit's localized descriptions. */
static NSString *ax_words(NSString *name) {
    if ([name hasPrefix:@"AX"]) name = [name substringFromIndex:2];
    NSMutableString *out = [NSMutableString string];
    for (NSUInteger i = 0; i < name.length; i++) {
        unichar ch = [name characterAtIndex:i];
        BOOL upper = ch >= 'A' && ch <= 'Z';
        if (upper && i > 0) [out appendString:@" "];
        unichar lower = upper ? (unichar)(ch - 'A' + 'a') : ch;
        [out appendFormat:@"%C", lower];
    }
    return out;
}
NSString *NSAccessibilityRoleDescription(NSString *role, NSString *subrole) { return role ? ax_words(subrole ?: role) : nil; }
NSString *NSAccessibilityActionDescription(NSString *action) { return action ? ax_words(action) : nil; }

/* The kill ring (AppKit's private one that text views use for Emacs-style kill and yank): one entry. */
static NSMutableString *g_kill_ring;
static BOOL g_kill_new_sequence = YES;
void _NSInitializeKillRing(void) {
    if (!g_kill_ring) g_kill_ring = [NSMutableString string];
}
void _NSAppendToKillRing(NSString *string) {
    _NSInitializeKillRing();
    if (g_kill_new_sequence) [g_kill_ring setString:@""];
    g_kill_new_sequence = NO;
    if (string) [g_kill_ring appendString:string];
}
void _NSPrependToKillRing(NSString *string) {
    _NSInitializeKillRing();
    if (g_kill_new_sequence) [g_kill_ring setString:@""];
    g_kill_new_sequence = NO;
    if (string) [g_kill_ring insertString:string atIndex:0];
}
NSString *_NSYankFromKillRing(void) {
    _NSInitializeKillRing();
    return [g_kill_ring copy];
}
void _NSNewKillRingSequence(void) { g_kill_new_sequence = YES; }
void _NSSetKillRingToYankedState(void) { g_kill_new_sequence = YES; }

/* NSFont's identity matrix (fontWithName:matrix:): AppKit exports a pointer to it. */
static const CGFloat hk_font_identity[6] = {1, 0, 0, 1, 0, 0};
const CGFloat *NSFontIdentityMatrix = hk_font_identity;
