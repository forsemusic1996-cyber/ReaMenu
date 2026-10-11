#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_GetResourcePath
#define REAPERAPI_WANT_AddCustomizableMenu
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
    // 32060 is REAPER's built-in MIDI Editor action section.
    // Use the section object supplied by REAPER rather than guessing
    // its display name, so this menu is registered for the MIDI Editor
    // section and not for REAPER's global main section.
    KbdSectionInfo* midiEditorSection =
        SectionFromUniqueID(32060);

    if (!midiEditorSection || !midiEditorSection->name)
    {
        Log(
            "ERROR: REAPER did not expose the built-in MIDI Editor "
            "section (32060)."
        );

        return false;
    }

    const char* midiEditorSectionName =
        midiEditorSection->name;

    Log(
        std::string("Using MIDI Editor section: '") +
        midiEditorSectionName +
        "' (32060)."
    );

    for (const MainMenu& mainMenu :
        g_menus)
    {
        if (!AddCustomizableMenu(
            mainMenu.id.c_str(),
            mainMenu.name.c_str(),
            midiEditorSectionName,
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
            "Registered MIDI Editor menu '" +
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
        CoUninitialize();
        return 0;
    }

    // Keep the registration structure so that the hook can
    // be removed when REAPER unloads the extension.
    g_rec = rec;

    CoUninitialize();

    return 1;
}
