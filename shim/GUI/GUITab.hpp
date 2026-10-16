#pragma once
// ---------------------------------------------------------------------------
// 上游 GUI/GUITab.hpp 的“断链修复版”。
//
// 为什么需要这个文件:
//   上游 GUI/GUITab.hpp 里有一行 `#include <GUI/GUI.hpp>`, 而 GUI/GUI.hpp 会拉进
//   <imgui.h> 和 GUI/Console.hpp -> <TextEditor.h>。
//   RE-UE4SS 的 deps/third/imgui 是个空的 submodule 占位目录(只有 CMakeLists.txt),
//   所以 TextEditor.h / imgui.h 在本机根本不存在, 编译必然 C1083。
//
//   但 GUITab 本身**完全不需要** GUI.hpp —— GUI.hpp 只是顺带定义了别的 UI 类型。
//   所以这里保留 GUITab 的定义逐字不变(布局必须与 UE4SS 编译时一致, 否则
//   CppUserModBase 里的 std::vector<std::shared_ptr<GUITab>> 会算错大小),
//   只把那一行断链的 include 去掉。
//
// ⚠ 改动本文件里 GUITab 的任何字段/顺序 = ABI 破坏。只允许删 include。
//
// 生效方式: CMakeLists.txt 把这个 shim/ 放在 UE4SS include 目录**之前**,
// 所以编译器在这里先找到 GUI/GUITab.hpp, 不会去碰上游那份。
// ---------------------------------------------------------------------------

#include <memory>
#include <utility>

#include <File/Macros.hpp>

namespace RC
{
    class CppUserModBase;
}

namespace RC::GUI
{
    class GUITab
    {
        friend class DebuggingGUI;

      public:
        using RenderFunctionType = void (*)(CppUserModBase*);

      private:
        RenderFunctionType render_function{};
        CppUserModBase* owner{};
        StringType tab_name{};

      public:
        GUITab() = delete;
        GUITab(StringViewType name, RenderFunctionType render_function) : tab_name(name), render_function(render_function){};
        GUITab(StringViewType name, RenderFunctionType render_function, CppUserModBase* owner)
            : tab_name(name), render_function(render_function), owner(owner){};
        ~GUITab() = default;

      private:
        auto get_owner() -> CppUserModBase*
        {
            return owner;
        }
    };
} // namespace RC::GUI
