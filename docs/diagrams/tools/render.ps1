# docs/diagrams/src/*.mmd 를 같은 폴더 기준 ../images 의 .svg, .png 로 변환한다.
# 외부 서비스 없이 mermaid.min.js 와 설치된 Chrome 헤드리스만 쓴다.
# 사용: powershell -ExecutionPolicy Bypass -File docs\diagrams\tools\render.ps1 [이름]
param([string]$Only)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$chrome = 'C:\Program Files\Google\Chrome\Application\chrome.exe'
$mermaid = (Resolve-Path (Join-Path $PSScriptRoot 'mermaid.min.js')).Path -replace '\\', '/'
$work = Join-Path $env:TEMP 'try_mmo_diagram_render'
$images = Join-Path $root 'images'
$utf8 = New-Object System.Text.UTF8Encoding $false

New-Item -ItemType Directory -Force $work, $images | Out-Null

function Invoke-Chrome([string[]]$arguments) {
    $common = @('--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
        '--allow-file-access-from-files', "--user-data-dir=`"$work\profile`"")
    $process = Start-Process $chrome -ArgumentList ($common + $arguments) -Wait -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput "$work\out.txt" -RedirectStandardError "$work\err.txt"
    return [IO.File]::ReadAllText("$work\out.txt", [Text.Encoding]::UTF8)
}

$sources = Get-ChildItem (Join-Path $root 'src') -Filter *.mmd
if ($Only) { $sources = $sources | Where-Object { $_.BaseName -eq $Only } }

foreach ($source in $sources) {
    $text = [IO.File]::ReadAllText($source.FullName, [Text.Encoding]::UTF8)
    $encoded = [Convert]::ToBase64String($utf8.GetBytes($text))

    $page = @"
<!doctype html><html><head><meta charset="utf-8"></head><body>
<div id="out"></div><pre id="xml"></pre><pre id="size"></pre>
<script src="file:///$mermaid"></script>
<script>
(async () => {
  const bytes = Uint8Array.from(atob('$encoded'), c => c.charCodeAt(0));
  const source = new TextDecoder().decode(bytes);
  mermaid.initialize({ startOnLoad: false, theme: 'default', securityLevel: 'loose',
    fontFamily: 'Malgun Gothic, sans-serif', htmlLabels: false,
    flowchart: { htmlLabels: false }, class: { htmlLabels: false } });
  try {
    const { svg } = await mermaid.render('diagram', source);
    document.getElementById('out').innerHTML = svg;
    const element = document.querySelector('#out svg');
    const box = element.viewBox.baseVal;
    element.setAttribute('width', box.width);
    element.setAttribute('height', box.height);
    element.style.maxWidth = '';
    element.style.backgroundColor = 'white';
    document.getElementById('size').textContent = Math.ceil(box.width) + 'x' + Math.ceil(box.height);
    document.getElementById('xml').textContent = new XMLSerializer().serializeToString(element);
  } catch (error) {
    document.getElementById('size').textContent = 'ERROR ' + error;
  }
})();
</script></body></html>
"@
    $pagePath = Join-Path $work "$($source.BaseName).html"
    [IO.File]::WriteAllText($pagePath, $page, $utf8)

    $dom = Invoke-Chrome @('--virtual-time-budget=15000', '--dump-dom', "`"file:///$($pagePath -replace '\\', '/')`"")
    $size = [regex]::Match($dom, '<pre id="size">([^<]*)</pre>').Groups[1].Value
    if ($size -notmatch '^(\d+)x(\d+)$') { throw "$($source.Name) 변환 실패: $size" }
    $width = [int]$Matches[1]
    $height = [int]$Matches[2]

    $xml = [regex]::Match($dom, '<pre id="xml">([\s\S]*?)</pre>').Groups[1].Value
    $svg = [System.Net.WebUtility]::HtmlDecode($xml)
    $svgPath = Join-Path $images "$($source.BaseName).svg"
    [IO.File]::WriteAllText($svgPath, $svg, $utf8)

    $viewPath = Join-Path $work "$($source.BaseName)_view.html"
    [IO.File]::WriteAllText($viewPath,
        "<!doctype html><html><head><meta charset=`"utf-8`"><style>html,body{margin:0;background:#fff}svg{display:block}</style></head><body>$svg</body></html>",
        $utf8)
    $pngPath = Join-Path $images "$($source.BaseName).png"
    Invoke-Chrome @('--force-device-scale-factor=2', '--hide-scrollbars', "--window-size=$width,$($height + 90)",
        "--screenshot=`"$pngPath`"", "`"file:///$($viewPath -replace '\\', '/')`"") | Out-Null

    "$($source.BaseName): ${width}x${height}"
}
