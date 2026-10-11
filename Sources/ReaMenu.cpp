#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_GetResourcePath
#define REAPERAPI_WANT_AddCustomizableMenu
#define REAPERAPI_WANT_MIDIEditor_GetActive
#define REAPERAPI_WANT_SectionFromUniqueID
#define REAPERAPI_WANT_NamedCommandLookup
#define REAPERAPI_IMPLEMENT

#include "SDK/reaper_plugin.h"
#include "SDK/reaper_plugin_functions.h"

#include <windows.h>
#include <msxml6.h>

#include <string>
#include <vector>
#include <fstream>
#include <cstdlib>
#include <cerrno>
#include <cctype>

// Link with MSXML 6.0
#pragma comment(lib, "msxml6.lib")


// ============================================================
// Data structures
// ============================================================

struct MenuItem
{
    enum Type
    {
        ACTION,
        SEPARATOR,
        SUBMENU
    };

    Type type;

    std::string name;
    std::string command;

    std::vector<MenuItem> children;
};


struct MainMenu
{
    std::string name;
    std::string id;

    std::vector<MenuItem> children;
};


// Parsed configuration.
static std::vector<MainMenu> g_menus;


// REAPER plugin registration information.
// Kept so the hook can be unregistered when REAPER unloads the DLL.
static reaper_plugin_info_t* g_rec = nullptr;

// True only after hookcustommenu has been successfully registered.
static bool g_hookRegistered = false;

struct InstalledMidiMenu
{
    HWND editor = nullptr;
    HMENU menuBar = nullptr;
    HMENU submenu = nullptr;
    std::string id;
};

static std::vector<InstalledMidiMenu> g_installedMidiMenus;
static bool g_timerRegistered = false;
static FILETIME g_menuIniWriteTime = {};
static bool g_menuIniExists = false;
static bool g_menuIniTimeInitialized = false;


// ============================================================
// Logging
// ============================================================

static std::string GetLogPath()
{
    const char* resourcePath =
        GetResourcePath();

    if (!resourcePath)
        return "ReaMenu.log";

    return std::string(resourcePath) +
        "\\ReaMenu.log";
}


static void ResetLog()
{
    std::ofstream file(
        GetLogPath(),
        std::ios::out | std::ios::trunc
    );
}


static void Log(const std::string& text)
{
    std::ofstream file(
        GetLogPath(),
        std::ios::out | std::ios::app
    );

    if (!file)
        return;

    file << text << std::endl;
}


// ============================================================
// UTF-16 -> UTF-8
// ============================================================

static std::string WideToUTF8(
    const wchar_t* text)
{
    if (!text)
        return std::string();

    int size =
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            -1,
            nullptr,
            0,
            nullptr,
            nullptr
        );

    if (size <= 1)
        return std::string();

    std::string result(
        size - 1,
        '\0'
    );

    WideCharToMultiByte(
        CP_UTF8,
        0,
        text,
        -1,
        &result[0],
        size,
        nullptr,
        nullptr
    );

    return result;
}


// ============================================================
// UTF-8 -> UTF-16
// ============================================================

static std::wstring UTF8ToWide(
    const std::string& text)
{
    if (text.empty())
        return std::wstring();

    int size =
        MultiByteToWideChar(
            CP_UTF8,
            0,
            text.c_str(),
            -1,
            nullptr,
            0
        );

    if (size <= 1)
        return std::wstring();

    std::wstring result(
        size - 1,
        L'\0'
    );

    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.c_str(),
        -1,
        &result[0],
        size
    );

    return result;
}


// ============================================================
// Get XML attribute
// ============================================================

static std::string GetAttribute(
    IXMLDOMNode* node,
    const wchar_t* attributeName)
{
    if (!node || !attributeName)
        return std::string();

    IXMLDOMNamedNodeMap* attributes =
        nullptr;

    HRESULT hr =
        node->get_attributes(
            &attributes
        );

    if (FAILED(hr) || !attributes)
        return std::string();

    BSTR name =
        SysAllocString(
            attributeName
        );

    if (!name)
    {
        attributes->Release();
        return std::string();
    }

    IXMLDOMNode* attribute =
        nullptr;

    hr =
        attributes->getNamedItem(
            name,
            &attribute
        );

    SysFreeString(name);
    attributes->Release();

    if (FAILED(hr) || !attribute)
        return std::string();

    BSTR value =
        nullptr;

    hr =
        attribute->get_text(
            &value
        );

    attribute->Release();

    if (FAILED(hr) || !value)
        return std::string();

    std::string result =
        WideToUTF8(value);

    SysFreeString(value);

    return result;
}


// ============================================================
// Parse <action>
// ============================================================

static bool ParseAction(
    IXMLDOMNode* node,
    MenuItem& item)
{
    std::string command =
        GetAttribute(
            node,
            L"command"
        );

    if (command.empty())
    {
        Log(
            "ERROR: <action> is missing "
            "the 'command' attribute."
        );

        return false;
    }

    item.type =
        MenuItem::ACTION;

    item.command =
        command;

    // Optional custom display name.
    item.name =
        GetAttribute(
            node,
            L"name"
        );

    return true;
}


// ============================================================
// Parse <separator>
// ============================================================

static bool ParseSeparator(
    IXMLDOMNode* node,
    MenuItem& item)
{
    item.type =
        MenuItem::SEPARATOR;

    return true;
}


// ============================================================
// Parse <menu> recursively
// ============================================================

static bool ParseMenu(
    IXMLDOMNode* node,
    MenuItem& item)
{
    std::string name =
        GetAttribute(
            node,
            L"name"
        );

    if (name.empty())
    {
        Log(
            "ERROR: <menu> is missing "
            "the 'name' attribute."
        );

        return false;
    }

    item.type =
        MenuItem::SUBMENU;

    item.name =
        name;

    IXMLDOMNodeList* children =
        nullptr;

    HRESULT hr =
        node->get_childNodes(
            &children
        );

    if (FAILED(hr) || !children)
    {
        Log(
            "ERROR: Unable to read children "
            "of <menu> '" +
            name +
            "'."
        );

        return false;
    }

    long count = 0;

    children->get_length(
        &count
    );

    for (long i = 0; i < count; ++i)
    {
        IXMLDOMNode* child =
            nullptr;

        if (FAILED(
            children->get_item(
                i,
                &child
            )) ||
            !child)
        {
            continue;
        }

        DOMNodeType nodeType;

        child->get_nodeType(
            &nodeType
        );

        // Ignore whitespace, comments and text.
        if (nodeType != NODE_ELEMENT)
        {
            child->Release();
            continue;
        }

        BSTR nodeName =
            nullptr;

        child->get_nodeName(
            &nodeName
        );

        if (!nodeName)
        {
            child->Release();
            continue;
        }

        MenuItem childItem;

        bool valid = true;

        if (_wcsicmp(
            nodeName,
            L"action") == 0)
        {
            valid =
                ParseAction(
                    child,
                    childItem
                );
        }
        else if (_wcsicmp(
            nodeName,
            L"separator") == 0)
        {
            valid =
                ParseSeparator(
                    child,
                    childItem
                );
        }
        else if (_wcsicmp(
            nodeName,
            L"menu") == 0)
        {
            valid =
                ParseMenu(
                    child,
                    childItem
                );
        }
        else
        {
            Log(
                "ERROR: Unknown element <" +
                WideToUTF8(nodeName) +
                "> inside <menu> '" +
                name +
                "'."
            );

            valid = false;
        }

        SysFreeString(
            nodeName
        );

        child->Release();

        if (!valid)
        {
            children->Release();
            return false;
        }

        item.children.push_back(
            childItem
        );
    }

    children->Release();

    return true;
}


// ============================================================
// Parse <mainmenu>
// ============================================================

static bool ParseMainMenu(
    IXMLDOMNode* node,
    MainMenu& mainMenu)
{
    mainMenu.name =
        GetAttribute(
            node,
            L"name"
        );

    mainMenu.id =
        GetAttribute(
            node,
            L"id"
        );

    if (mainMenu.name.empty())
    {
        Log(
            "ERROR: <mainmenu> is missing "
            "the 'name' attribute."
        );

        return false;
    }

    if (mainMenu.id.empty())
    {
        Log(
            "ERROR: <mainmenu> '" +
            mainMenu.name +
            "' is missing "
            "the 'id' attribute."
        );

        return false;
    }

    IXMLDOMNodeList* children =
        nullptr;

    HRESULT hr =
        node->get_childNodes(
            &children
        );

    if (FAILED(hr) || !children)
    {
        Log(
            "ERROR: Unable to read children "
            "of <mainmenu> '" +
            mainMenu.name +
            "'."
        );

        return false;
    }

    long count = 0;

    children->get_length(
        &count
    );

    for (long i = 0; i < count; ++i)
    {
        IXMLDOMNode* child =
            nullptr;

        if (FAILED(
            children->get_item(
                i,
                &child
            )) ||
            !child)
        {
            continue;
        }

        DOMNodeType nodeType;

        child->get_nodeType(
            &nodeType
        );

        if (nodeType != NODE_ELEMENT)
        {
            child->Release();
            continue;
        }

        BSTR nodeName =
            nullptr;

        child->get_nodeName(
            &nodeName
        );

        if (!nodeName)
        {
            child->Release();
            continue;
        }

        MenuItem item;

        bool valid = true;

        if (_wcsicmp(
            nodeName,
            L"action") == 0)
        {
            valid =
                ParseAction(
                    child,
                    item
                );
        }
        else if (_wcsicmp(
            nodeName,
            L"separator") == 0)
        {
            valid =
                ParseSeparator(
                    child,
                    item
                );
        }
        else if (_wcsicmp(
            nodeName,
            L"menu") == 0)
        {
            valid =
                ParseMenu(
                    child,
                    item
                );
        }
        else
        {
            Log(
                "ERROR: Unknown element <" +
                WideToUTF8(nodeName) +
                "> inside <mainmenu> '" +
                mainMenu.name +
                "'."
            );

            valid = false;
        }

        SysFreeString(
            nodeName
        );

        child->Release();

        if (!valid)
        {
            children->Release();
            return false;
        }

        mainMenu.children.push_back(
            item
        );
    }

    children->Release();

    return true;
}


// ============================================================
// Load configuration
// ============================================================

static bool LoadConfiguration()
{
    g_menus.clear();
    ResetLog();

    const char* resourcePath =
        GetResourcePath();

    if (!resourcePath)
    {
        Log(
            "ERROR: GetResourcePath() returned null."
        );

        return false;
    }

    std::string xmlPath =
        std::string(resourcePath) +
        "\\ReaMenu.xml";

    Log("");
    Log("========================================");
    Log("Loading ReaMenu.xml");
    Log("Path: " + xmlPath);

    // --------------------------------------------------------
    // Create MSXML document.
    // --------------------------------------------------------

    IXMLDOMDocument2* document =
        nullptr;

    HRESULT hr =
        CoCreateInstance(
            CLSID_DOMDocument60,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IXMLDOMDocument2,
            reinterpret_cast<void**>(
                &document
                )
        );

    if (FAILED(hr) || !document)
    {
        Log(
            "ERROR: Unable to create "
            "MSXML 6.0 DOM document."
        );

        return false;
    }

    document->put_async(
        VARIANT_FALSE
    );

    document->put_validateOnParse(
        VARIANT_FALSE
    );

    document->put_resolveExternals(
        VARIANT_FALSE
    );

    // --------------------------------------------------------
    // Convert path to UTF-16.
    // --------------------------------------------------------

    std::wstring widePath =
        UTF8ToWide(xmlPath);

    if (widePath.empty())
    {
        Log(
            "ERROR: Unable to convert "
            "XML path to UTF-16."
        );

        document->Release();

        return false;
    }

    BSTR filename =
        SysAllocString(
            widePath.c_str()
        );

    if (!filename)
    {
        Log(
            "ERROR: Unable to allocate "
            "XML filename BSTR."
        );

        document->Release();

        return false;
    }

    VARIANT source;

    VariantInit(
        &source
    );

    source.vt =
        VT_BSTR;

    source.bstrVal =
        filename;

    VARIANT_BOOL loaded =
        VARIANT_FALSE;

    hr =
        document->load(
            source,
            &loaded
        );

    VariantClear(
        &source
    );

    if (FAILED(hr) ||
        loaded != VARIANT_TRUE)
    {
        Log(
            "ERROR: Unable to load XML file."
        );

        IXMLDOMParseError* parseError =
            nullptr;

        if (SUCCEEDED(
            document->get_parseError(
                &parseError
            )) &&
            parseError)
        {
            BSTR reason =
                nullptr;

            if (SUCCEEDED(
                parseError->get_reason(
                    &reason
                )) &&
                reason)
            {
                Log(
                    "XML parser error: " +
                    WideToUTF8(reason)
                );

                SysFreeString(
                    reason
                );
            }

            parseError->Release();
        }

        document->Release();

        return false;
    }

    // --------------------------------------------------------
    // Root element.
    // --------------------------------------------------------

    IXMLDOMElement* root =
        nullptr;

    hr =
        document->get_documentElement(
            &root
        );

    if (FAILED(hr) || !root)
    {
        Log(
            "ERROR: XML document has "
            "no root element."
        );

        document->Release();

        return false;
    }

    BSTR rootName =
        nullptr;

    root->get_nodeName(
        &rootName
    );

    bool validRoot =
        rootName &&
        _wcsicmp(
            rootName,
            L"menus"
        ) == 0;

    if (rootName)
        SysFreeString(
            rootName
        );

    if (!validRoot)
    {
        Log(
            "ERROR: Root element "
            "must be <menus>."
        );

        root->Release();
        document->Release();

        return false;
    }

    // --------------------------------------------------------
    // Parse all <mainmenu>.
    // --------------------------------------------------------

    IXMLDOMNodeList* children =
        nullptr;

    hr =
        root->get_childNodes(
            &children
        );

    if (FAILED(hr) || !children)
    {
        Log(
            "ERROR: Unable to read <menus>."
        );

        root->Release();
        document->Release();

        return false;
    }

    long count = 0;

    children->get_length(
        &count
    );

    for (long i = 0; i < count; ++i)
    {
        IXMLDOMNode* child =
            nullptr;

        if (FAILED(
            children->get_item(
                i,
                &child
            )) ||
            !child)
        {
            continue;
        }

        DOMNodeType nodeType;

        child->get_nodeType(
            &nodeType
        );

        if (nodeType != NODE_ELEMENT)
        {
            child->Release();
            continue;
        }

        BSTR nodeName =
            nullptr;

        child->get_nodeName(
            &nodeName
        );

        if (!nodeName)
        {
            child->Release();
            continue;
        }

        if (_wcsicmp(
            nodeName,
            L"mainmenu") != 0)
        {
            Log(
                "ERROR: Unexpected element <" +
                WideToUTF8(nodeName) +
                "> directly inside <menus>."
            );

            SysFreeString(
                nodeName
            );

            child->Release();
            children->Release();
            root->Release();
            document->Release();

            g_menus.clear();

            return false;
        }

        MainMenu mainMenu;

        bool valid =
            ParseMainMenu(
                child,
                mainMenu
            );

        SysFreeString(
            nodeName
        );

        child->Release();

        if (!valid)
        {
            children->Release();
            root->Release();
            document->Release();

            g_menus.clear();

            return false;
        }

        // IDs must be unique.
        for (const MainMenu& existing :
            g_menus)
        {
            if (existing.id ==
                mainMenu.id)
            {
                Log(
                    "ERROR: Duplicate "
                    "mainmenu id '" +
                    mainMenu.id +
                    "'."
                );

                children->Release();
                root->Release();
                document->Release();

                g_menus.clear();

                return false;
            }
        }

        g_menus.push_back(
            mainMenu
        );
    }

    children->Release();
    root->Release();
    document->Release();

    if (g_menus.empty())
    {
        Log(
            "ERROR: XML contains "
            "no <mainmenu>."
        );

        return false;
    }

    Log(
        "XML loaded successfully. "
        "Main menus: " +
        std::to_string(
            g_menus.size()
        )
    );

    return true;
}


// ============================================================
// Resolve action command
// ============================================================

static int ResolveCommand(
    const std::string& command)
{
    if (command.empty())
        return 0;

    // --------------------------------------------------------
    // Numeric native REAPER command.
    // --------------------------------------------------------

    char* end = nullptr;

    errno = 0;

    long numeric =
        std::strtol(
            command.c_str(),
            &end,
            10
        );

    if (errno == 0 &&
        end &&
        *end == '\0' &&
        numeric > 0 &&
        numeric <= 0x7fffffff)
    {
        return static_cast<int>(
            numeric
            );
    }

    // --------------------------------------------------------
    // Named command such as _RS... or _SWS...
    // --------------------------------------------------------

    int commandId =
        NamedCommandLookup(
            command.c_str()
        );

    if (commandId == 0)
    {
        Log(
            "WARNING: Unknown command '" +
            command +
            "'."
        );

        return 0;
    }

    return commandId;
}


// ============================================================
// Insert one menu item
// ============================================================

static void InsertMenuItem(
    HMENU menu,
    const MenuItem& item)
{
    if (!menu)
        return;

    // --------------------------------------------------------
    // Separator
    // --------------------------------------------------------

    if (item.type ==
        MenuItem::SEPARATOR)
    {
        InsertMenuW(
            menu,
            static_cast<UINT>(-1),
            MF_BYPOSITION |
            MF_SEPARATOR,
            0,
            nullptr
        );

        return;
    }

    // --------------------------------------------------------
    // Submenu
    // --------------------------------------------------------

    if (item.type ==
        MenuItem::SUBMENU)
    {
        HMENU submenu =
            CreatePopupMenu();

        if (!submenu)
        {
            Log(
                "ERROR: Unable to create "
                "submenu '" +
                item.name +
                "'."
            );

            return;
        }

        for (const MenuItem& child :
            item.children)
        {
            InsertMenuItem(
                submenu,
                child
            );
        }

        std::wstring wideName =
            UTF8ToWide(
                item.name
            );

        InsertMenuW(
            menu,
            static_cast<UINT>(-1),
            MF_BYPOSITION |
            MF_POPUP,
            reinterpret_cast<UINT_PTR>(
                submenu
                ),
            wideName.c_str()
        );

        return;
    }

    // --------------------------------------------------------
    // Action
    // --------------------------------------------------------

    if (item.type ==
        MenuItem::ACTION)
    {
        int commandId =
            ResolveCommand(
                item.command
            );

        if (commandId == 0)
            return;

        std::string displayName =
            item.name.empty()
            ? item.command
            : item.name;

        std::wstring wideName =
            UTF8ToWide(
                displayName
            );

        InsertMenuW(
            menu,
            static_cast<UINT>(-1),
            MF_BYPOSITION |
            MF_STRING,
            static_cast<UINT>(
                commandId
                ),
            wideName.c_str()
        );
    }
}


// ============================================================
// Populate a REAPER menu
// ============================================================

static void PopulateMenu(
    HMENU menu,
    const MainMenu& mainMenu)
{
    if (!menu)
        return;

    for (const MenuItem& item :
        mainMenu.children)
    {
        InsertMenuItem(
            menu,
            item
        );
    }
}


static std::string Trim(
    const std::string& value)
{
    size_t first = 0;

    while (first < value.size() &&
           std::isspace(
               static_cast<unsigned char>(
                   value[first]
               )))
    {
        ++first;
    }

    size_t last = value.size();

    while (last > first &&
           std::isspace(
               static_cast<unsigned char>(
                   value[last - 1]
               )))
    {
        --last;
    }

    return value.substr(first, last - first);
}


static std::string GetMenuIniPath()
{
    const char* resourcePath =
        GetResourcePath();

    if (!resourcePath)
        return std::string();

    return std::string(resourcePath) +
        "\\reaper-menu.ini";
}


static bool ReadCustomizedMenuItems(
    const std::string& sectionName,
    std::vector<MenuItem>& items)
{
    items.clear();

    std::string path =
        GetMenuIniPath();

    if (path.empty())
        return false;

    std::ifstream file(path);

    if (!file)
        return false;

    bool inSection = false;
    bool sectionFound = false;
    std::vector<std::vector<MenuItem>*> stack;
    stack.push_back(&items);

    std::string line;

    while (std::getline(file, line))
    {
        line = Trim(line);

        if (line.empty())
            continue;

        if (line.front() == '[' &&
            line.back() == ']')
        {
            std::string currentSection =
                line.substr(
                    1,
                    line.size() - 2
                );

            inSection =
                _stricmp(
                    currentSection.c_str(),
                    sectionName.c_str()
                ) == 0;

            if (inSection)
            {
                sectionFound = true;
                stack.clear();
                stack.push_back(&items);
            }

            continue;
        }

        if (!inSection ||
            line.rfind("item_", 0) != 0)
        {
            continue;
        }

        size_t equals = line.find('=');

        if (equals == std::string::npos)
            continue;

        std::string value =
            Trim(line.substr(equals + 1));

        if (value == "-1")
        {
            MenuItem separator;
            separator.type = MenuItem::SEPARATOR;
            stack.back()->push_back(separator);
            continue;
        }

        if (value == "-3")
        {
            if (stack.size() > 1)
                stack.pop_back();

            continue;
        }

        if (value.rfind("-2", 0) == 0 &&
            (value.size() == 2 ||
             std::isspace(
                 static_cast<unsigned char>(
                     value[2]
                 ))))
        {
            std::string submenuName =
                Trim(value.substr(2));

            if (submenuName.empty())
                continue;

            MenuItem submenu;
            submenu.type = MenuItem::SUBMENU;
            submenu.name = submenuName;
            stack.back()->push_back(submenu);
            stack.push_back(
                &stack.back()->back().children
            );

            continue;
        }

        size_t separator =
            value.find_first_of(" \t");

        std::string command =
            separator == std::string::npos
            ? value
            : value.substr(0, separator);

        std::string displayName =
            separator == std::string::npos
            ? std::string()
            : Trim(value.substr(separator + 1));

        if (command.empty())
            continue;

        MenuItem action;
        action.type = MenuItem::ACTION;
        action.command = command;
        action.name = displayName;
        stack.back()->push_back(action);
    }

    return sectionFound;
}


static bool LoadCustomizedItems(
    const MainMenu& mainMenu,
    std::vector<MenuItem>& items)
{
    if (ReadCustomizedMenuItems(
        mainMenu.id,
        items))
    {
        return true;
    }

    // Preserve menus created by earlier versions of FSR ReaMenu.
    if (mainMenu.id == "midi" &&
        ReadCustomizedMenuItems(
            "fsr_reamenu_midi",
            items))
    {
        Log(
            "Using legacy customized menu section "
            "[fsr_reamenu_midi]."
        );

        return true;
    }

    return false;
}


static void PopulateDirectMidiMenu(
    HMENU menu,
    const MainMenu& mainMenu)
{
    std::vector<MenuItem> configuredItems;

    if (LoadCustomizedItems(
        mainMenu,
        configuredItems))
    {
        for (const MenuItem& item :
            configuredItems)
        {
            InsertMenuItem(menu, item);
        }

        return;
    }

    PopulateMenu(menu, mainMenu);
}


static bool MenuIniChanged()
{
    std::string path =
        GetMenuIniPath();

    WIN32_FILE_ATTRIBUTE_DATA data = {};

    bool exists =
        !path.empty() &&
        GetFileAttributesExA(
            path.c_str(),
            GetFileExInfoStandard,
            &data
        ) != FALSE;

    bool changed =
        !g_menuIniTimeInitialized ||
        exists != g_menuIniExists;

    if (exists &&
        g_menuIniTimeInitialized &&
        CompareFileTime(
            &data.ftLastWriteTime,
            &g_menuIniWriteTime
        ) != 0)
    {
        changed = true;
    }

    g_menuIniExists = exists;

    if (exists)
        g_menuIniWriteTime = data.ftLastWriteTime;
    else
        g_menuIniWriteTime = {};

    g_menuIniTimeInitialized = true;

    return changed;
}


// ============================================================
// Direct MIDI Editor menu-bar integration
// ============================================================

static bool HasSubmenu(
    HMENU menuBar,
    HMENU submenu)
{
    if (!menuBar || !submenu)
        return false;

    int count = GetMenuItemCount(menuBar);

    for (int i = 0; i < count; ++i)
    {
        if (GetSubMenu(menuBar, i) == submenu)
            return true;
    }

    return false;
}


static void RemoveInstalledMenu(
    InstalledMidiMenu& installed)
{
    if (installed.menuBar && installed.submenu)
    {
        int count = GetMenuItemCount(installed.menuBar);

        for (int i = 0; i < count; ++i)
        {
            if (GetSubMenu(installed.menuBar, i) == installed.submenu)
            {
                DeleteMenu(
                    installed.menuBar,
                    static_cast<UINT>(i),
                    MF_BYPOSITION
                );

                if (installed.editor)
                    DrawMenuBar(installed.editor);

                break;
            }
        }

        DestroyMenu(installed.submenu);
    }

    installed.editor = nullptr;
    installed.menuBar = nullptr;
    installed.submenu = nullptr;
    installed.id.clear();
}


static void RemoveAllInstalledMenus()
{
    for (InstalledMidiMenu& installed :
        g_installedMidiMenus)
    {
        RemoveInstalledMenu(installed);
    }

    g_installedMidiMenus.clear();
}


static void InstallMenuInMidiEditor(
    HWND midiEditor,
    const MainMenu& mainMenu)
{
    if (!midiEditor || !IsWindow(midiEditor))
        return;

    HMENU menuBar = GetMenu(midiEditor);

    if (!menuBar)
    {
        Log(
            "MIDI Editor window has no menu bar. HWND=" +
            std::to_string(
                reinterpret_cast<UINT_PTR>(midiEditor)
            )
        );

        return;
    }

    for (const InstalledMidiMenu& installed :
        g_installedMidiMenus)
    {
        if (installed.editor == midiEditor &&
            installed.menuBar == menuBar &&
            installed.id == mainMenu.id &&
            installed.submenu &&
            HasSubmenu(menuBar, installed.submenu))
        {
            return;
        }
    }

    HMENU submenu = CreatePopupMenu();

    if (!submenu)
    {
        Log(
            "ERROR: CreatePopupMenu() failed for MIDI Editor menu '" +
            mainMenu.name +
            "'."
        );

        return;
    }

    PopulateDirectMidiMenu(submenu, mainMenu);

    std::wstring wideName =
        UTF8ToWide(mainMenu.name);

    if (!AppendMenuW(
        menuBar,
        MF_POPUP,
        reinterpret_cast<UINT_PTR>(submenu),
        wideName.c_str()))
    {
        DestroyMenu(submenu);

        Log(
            "ERROR: AppendMenuW() failed for MIDI Editor menu '" +
            mainMenu.name +
            "'."
        );

        return;
    }

    DrawMenuBar(midiEditor);

    InstalledMidiMenu installed;
    installed.editor = midiEditor;
    installed.menuBar = menuBar;
    installed.submenu = submenu;
    installed.id = mainMenu.id;
    g_installedMidiMenus.push_back(installed);

    Log(
        "Installed MIDI Editor menu-bar item '" +
        mainMenu.name +
        "' (" +
        mainMenu.id +
        ")"
    );
}


static void InstallMenusInActiveMidiEditor()
{
    if (MenuIniChanged())
        RemoveAllInstalledMenus();

    HWND midiEditor = MIDIEditor_GetActive();

    if (!midiEditor)
        return;

    for (const MainMenu& mainMenu :
        g_menus)
    {
        InstallMenuInMidiEditor(
            midiEditor,
            mainMenu
        );
    }
}


static void MidiMenuTimer()
{
    InstallMenusInActiveMidiEditor();
}


// ============================================================
// REAPER custom menu hook
// ============================================================

static void MenuHook(
    const char* menu_id,
    void* menu,
    int flag)
{
    if (!menu_id || !menu)
        return;

    // Only build the default menu structure.
    if (flag != 0)
        return;

    HMENU hMenu =
        static_cast<HMENU>(
            menu
            );

    for (const MainMenu& mainMenu :
        g_menus)
    {
        if (mainMenu.id ==
            menu_id)
        {
            Log(
                "Building menu '" +
                mainMenu.name +
                "' (" +
                mainMenu.id +
                ")"
            );

            PopulateMenu(
                hMenu,
                mainMenu
            );

            return;
        }
    }
}


// ============================================================
// Register all XML main menus
// ============================================================

static bool RegisterMenus(
    reaper_plugin_info_t* rec)
{
    KbdSectionInfo* midiEditorSection =
        SectionFromUniqueID(32060);

    if (!midiEditorSection ||
        !midiEditorSection->name)
    {
        Log(
            "ERROR: REAPER did not expose the MIDI Editor "
            "keyboard section (32060)."
        );

        return false;
    }

    Log(
        std::string("Using MIDI Editor Customize section '") +
        midiEditorSection->name +
        "' (32060)."
    );

    for (const MainMenu& mainMenu :
        g_menus)
    {
        if (!AddCustomizableMenu(
            mainMenu.id.c_str(),
            mainMenu.name.c_str(),
            midiEditorSection->name,
            true))
        {
            Log(
                "ERROR: AddCustomizableMenu() "
                "failed for '" +
                mainMenu.id +
                "'."
            );

            return false;
        }

        Log(
            "Registered customizable MIDI menu '" +
            mainMenu.name +
            "' (" +
            mainMenu.id +
            ")"
        );
    }

    if (!rec->Register(
        "hookcustommenu",
        reinterpret_cast<void*>(
            MenuHook
            )))
    {
        Log(
            "ERROR: Unable to register "
            "hookcustommenu."
        );

        return false;
    }

    g_hookRegistered = true;

    Log(
        "hookcustommenu registered."
    );

    g_rec = rec;

    return true;
}


// ============================================================
// REAPER entry point
// ============================================================

extern "C"
REAPER_PLUGIN_DLL_EXPORT
int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE hInstance,
    reaper_plugin_info_t* rec)
{
    // --------------------------------------------------------
    // Plugin unload.
    //
    // REAPER calls the entry point with rec == nullptr when
    // the extension is being unloaded.
    // --------------------------------------------------------

    if (!rec)
    {
        if (g_rec && g_hookRegistered)
        {
            if (g_rec->Register(
                "-hookcustommenu",
                reinterpret_cast<void*>(
                    MenuHook
                    )))
            {
                Log(
                    "hookcustommenu unregistered."
                );
            }
            else
            {
                Log(
                    "WARNING: Unable to unregister "
                    "hookcustommenu."
                );
            }
        }

        g_hookRegistered = false;
        g_rec = nullptr;
        g_menus.clear();

        return 0;
    }

    // --------------------------------------------------------
    // Plugin initialization.
    // --------------------------------------------------------

    int loadResult =
        REAPERAPI_LoadAPI(
            rec->GetFunc
        );

    if (loadResult != 0)
        return 0;

    HRESULT hr =
        CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED
        );

    bool comInitialized =
        SUCCEEDED(hr) ||
        hr == S_FALSE;

    if (!comInitialized)
    {
        Log(
            "ERROR: COM initialization failed."
        );

        return 0;
    }

    bool success =
        LoadConfiguration();

    if (!success)
    {
        CoUninitialize();
        return 1;
    }

    if (!RegisterMenus(rec))
    {
        g_rec = nullptr;
        CoUninitialize();
        return 0;
    }

    CoUninitialize();

    return 1;
}
