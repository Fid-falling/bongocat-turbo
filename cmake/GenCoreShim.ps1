# 从官方 Cubism Core 头文件生成"运行期转发 shim"。
#
# 为什么需要它(法律边界的技术落地): 发行物里不得包含 Live2D Cubism Core —— 它是专有库,
# 只能由用户自己从官网取得、在运行期加载。但 Cubism Framework 是**源码**, 它必须能编译,
# 而它调用的是 csm* 这一整套 API。所以做法是: 由生成的 shim 声明并实现全部 csm* 函数,
# 每个函数在第一次被调用时去宿主那里解析真实符号(宿主加载用户自备的 Core), 解析不到就
# 返回中性值 —— 于是宿主/组件在**不链接 Core 的前提下**依然能编译、链接、启动并优雅降级。
#
# 只用官方头文件作为输入(不读 .lib/.a, 更不读 DLL): 头文件是接口描述, 不含实现。
#
# 生成期健全性检查(任一不过就失败, 而不是生成一个"看起来正常"的空 shim):
#   * 声明数 >= 40: Core 5-r.5 有 55 个; 少于 40 说明头文件格式变了, 需要人来看;
#   * 函数名不重复;
#   * 参数里没有内联函数指针/数组/可变参数(会破坏"去掉参数名"与 x86 名字装饰的假设);
#   * 参数类型里没有 double/结构体按值传递(同上)。
param(
    [Parameter(Mandatory = $true)][string]$Header,
    [Parameter(Mandatory = $true)][string]$Output
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $Header -PathType Leaf)) {
    throw "Cubism Core header not found: $Header"
}
$text = ([IO.File]::ReadAllText($Header)) -replace "`r`n", "`n"
# 解析前先去掉注释: 头文件里有 `#if !defined(csmApi)` / `#define csmApi` 这种**宏**出现,
# 注释里也可能提到函数名。不剥注释就会从 `defined(csmApi)` 开始误匹配, 把一大段类型定义
# 当成返回类型(实测踩到过: 生成的函数返回类型是整段结构体定义 + 编译报 C1020)。
$parsed = [regex]::Replace($text, '(?s)/\*.*?\*/', '')
$parsed = [regex]::Replace($parsed, '(?m)^\s*//.*$', '')

# 只认"行首的 csmApi 声明": 宏里的 csmApi 不在行首。
$pattern = '(?m)^[ \t]*csmApi\s+(?<ret>[^;\r\n]+?)\s+csmCallingConvention\s+(?<name>csm[A-Za-z0-9_]*)\s*\((?<params>[^()]*)\)\s*;'
$declarations = @([regex]::Matches($parsed, $pattern))
if ($declarations.Count -lt 40) {
    throw ("The Cubism Core header exposes only {0} csmApi declarations; expected at least 40. " +
        "The header layout probably changed - refusing to generate a silently empty shim." -f $declarations.Count)
}

$seen = @{}
$functions = @()
foreach ($declaration in $declarations) {
    $name = $declaration.Groups['name'].Value
    $return = ($declaration.Groups['ret'].Value -replace '\s+', ' ').Trim()
    $parameters = $declaration.Groups['params'].Value.Trim()
    if ($return -match '[#;{}]' -or $return.Length -gt 64) {
        throw "The return type of $name parses as garbage ('$return') - the header layout changed"
    }
    if ($seen.ContainsKey($name)) { throw "Duplicate csmApi declaration: $name" }
    $seen[$name] = $true

    $types = @()
    $names = @()
    if ($parameters -and $parameters -ne 'void') {
        foreach ($raw in $parameters.Split(',')) {
            $parameter = ($raw -replace '\s+', ' ').Trim()
            if (-not $parameter) { throw "Empty parameter in $name" }
            if ($parameter -match '\.\.\.') { throw "Variadic parameter in $name is unsupported" }
            if ($parameter -match '[()\[\]]') { throw "Inline function pointer or array parameter in $name is unsupported" }
            if ($parameter -match '\bdouble\b|\blong long\b') { throw "Wide parameter in $name breaks the x86 name-mangling assumption" }
            $match = [regex]::Match($parameter, '^(?<type>.+?)\s*(?<arg>[A-Za-z_][A-Za-z0-9_]*)$')
            if (-not $match.Success) { throw "Cannot split the type from the name in '$parameter' ($name)" }
            $types += $match.Groups['type'].Value.Trim()
            $names += $match.Groups['arg'].Value
        }
    }
    $pointerReturn = ($return -match '\*') -or ($return -eq 'csmLogFunction')
    $functions += [pscustomobject]@{
        Name = $name; Return = $return; Types = $types; Args = $names; Pointer = $pointerReturn
        # 定义要用**带参数名**的形参表, 函数指针 typedef 要用**只有类型**的表: 两者不能混,
        # 混了就会生成 `ret f(const void*, const unsigned int) {...}` 这种没有名字的形参
        # (C2055), 函数体里再用 address/size 就报未声明。
        Parameters = $(if ($parameters -and $parameters -ne 'void') { ($parameters -replace '\s+', ' ') } else { 'void' })
    }
}

$builder = [System.Text.StringBuilder]::new()
[void]$builder.AppendLine('/*')
[void]$builder.AppendLine(' * 由 cmake/GenCoreShim.ps1 从官方 Core 头文件生成 —— 不要手改。')
[void]$builder.AppendLine(' *')
[void]$builder.AppendLine(' * 每个 csm* 函数在这里"存在"但**不实现**: 第一次被调用时向宿主解析真实符号')
[void]$builder.AppendLine(' * (宿主持有用户自备的 Core 动态库), 解析不到就返回中性值。这样宿主与渲染组件')
[void]$builder.AppendLine(' * 都能在不链接 Core 的前提下编译、链接、启动, 并在用户没导入 Core 时优雅降级。')
[void]$builder.AppendLine(' */')
[void]$builder.AppendLine('#include <Live2DCubismCore.h>')
[void]$builder.AppendLine('')
[void]$builder.AppendLine('#include <stddef.h>')
[void]$builder.AppendLine('#include <stdio.h>')
[void]$builder.AppendLine('')
[void]$builder.AppendLine('/* 宿主提供: 在已加载的用户 Core 里解析符号(见 live2d_core_paths.c)。 */')
[void]$builder.AppendLine('extern void *bongo_cat_cubism_core_resolve(const char *name);')
[void]$builder.AppendLine('')
[void]$builder.AppendLine('/*')
[void]$builder.AppendLine(' * x86 上 __stdcall 的导出名会被装饰成 _name@N(N 是参数字节数)。x64 只有一种调用约定,')
[void]$builder.AppendLine(' * 名字不加装饰, 所以这段回退在 x64 上永远不会命中 —— 留着是为了 x86 构建不至于直接失效。')
[void]$builder.AppendLine(' * 前提(生成期已断言): Core 的参数全是 <= 4 字节的标量或指针, 所以 N = 4 * 参数个数。')
[void]$builder.AppendLine(' */')
[void]$builder.AppendLine('static void *shim_resolve(const char *name, int argument_count) {')
[void]$builder.AppendLine('    void *symbol = bongo_cat_cubism_core_resolve(name);')
[void]$builder.AppendLine('#if defined(_M_IX86)')
[void]$builder.AppendLine('    if (!symbol) {')
[void]$builder.AppendLine('        char decorated[96];')
[void]$builder.AppendLine('        if (snprintf(decorated, sizeof(decorated), "_%s@%d", name, argument_count * 4) > 0)')
[void]$builder.AppendLine('            symbol = bongo_cat_cubism_core_resolve(decorated);')
[void]$builder.AppendLine('    }')
[void]$builder.AppendLine('#else')
[void]$builder.AppendLine('    (void)argument_count;')
[void]$builder.AppendLine('#endif')
[void]$builder.AppendLine('    return symbol;')
[void]$builder.AppendLine('}')
[void]$builder.AppendLine('')

foreach ($function in $functions) {
    $parameterList = if ($function.Types.Count) { ($function.Types -join ', ') } else { 'void' }
    $callList = $function.Args -join ', '
    [void]$builder.AppendLine(('{0} csmCallingConvention {1}({2})' -f $function.Return, $function.Name, $function.Parameters))
    [void]$builder.AppendLine('{')
    [void]$builder.AppendLine(('    typedef {0} (csmCallingConvention *shim_fn)({1});' -f $function.Return, $parameterList))
    [void]$builder.AppendLine('    /* 静态缓存 + 良性竞争: 最坏情况是多个线程各写一次同样的指针。 */')
    [void]$builder.AppendLine('    static shim_fn resolved;')
    [void]$builder.AppendLine(('    if (!resolved) resolved = (shim_fn)shim_resolve("{0}", {1});' -f $function.Name, $function.Args.Count))
    if ($function.Return -eq 'void') {
        [void]$builder.AppendLine(('    if (resolved) resolved({0});' -f $callList))
    } else {
        $neutral = if ($function.Pointer) { 'NULL' } else { '0' }
        [void]$builder.AppendLine(('    if (!resolved) return {0};' -f $neutral))
        [void]$builder.AppendLine(('    return resolved({0});' -f $callList))
    }
    [void]$builder.AppendLine('}')
    [void]$builder.AppendLine('')
}

$directory = Split-Path -Parent $Output
if ($directory -and -not (Test-Path -LiteralPath $directory)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
# 内容不变就不重写: 否则每次配置都会让下游重新编译整个 Framework(它包含这个头)。
$content = $builder.ToString()
if ((Test-Path -LiteralPath $Output) -and ([IO.File]::ReadAllText($Output) -eq $content)) {
    Write-Host "Cubism Core shim is up to date ($($functions.Count) symbols): $Output"
    return
}
[IO.File]::WriteAllText($Output, $content)
Write-Host "Generated the Cubism Core shim ($($functions.Count) symbols): $Output"
