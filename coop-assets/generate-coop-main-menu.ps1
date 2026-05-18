param(
    [string]$GameData = "C:\Program Files (x86)\Steam\steamapps\common\Jedi Outcast\GameData"
)

$ErrorActionPreference = "Stop"

$modUi = Join-Path $GameData "coop\ui"
$baseDir = Join-Path $GameData "base"
$sourcePk3s = @("assets5.pk3", "assets2.pk3", "assets1.pk3", "assets0.pk3") | ForEach-Object {
    Join-Path $baseDir $_
}
$repoUi = Join-Path $PSScriptRoot "ui"
$menuFiles = @("main.menu", "newgame.menu", "loadgame.menu", "controls.menu", "setup.menu")

if (-not ($sourcePk3s | Where-Object { Test-Path -LiteralPath $_ })) {
    throw "Could not find Jedi Outcast asset PK3s in $baseDir"
}

New-Item -ItemType Directory -Force -Path $modUi | Out-Null

Add-Type -AssemblyName System.IO.Compression.FileSystem

$coopBlock = @'
        // Big button "COOP"
        itemDef
        {
            name                coopgamebutton_glow
            group               coopglobal
            style               WINDOW_STYLE_SHADER
            rect                245 444 130 24
            background          "gfx/menus/menu_buttonback"           // Frame around button
            forecolor           1 1 1 1
            visible             0
            decoration
        }

        itemDef
        {
            name                coopgamebutton
            group               othermain
            text                "COOP"
            descText            "Invite a friend to the automatic Steam co-op lobby"
            type                ITEM_TYPE_BUTTON
            style               WINDOW_STYLE_EMPTY
            rect                245 444 130 24
            font                3
            textscale           1
            textalign           ITEM_ALIGN_CENTER
            textstyle           3
            textalignx          65
            textaligny          -1
            forecolor           0.65 0.65 1 1
            visible             1

            mouseEnter
            {
                show            coopgamebutton_glow
            }
            mouseExit
            {
                hide            coopgamebutton_glow
            }
            action
            {
                play            "sound/interface/button1.wav" ;
                close           all ;
                open            coopMenu
            }
        }

'@

function Read-MenuFromAssets {
    param([string]$MenuFile)

    $entryPath = "ui/$MenuFile"
    foreach ($pk3 in $sourcePk3s) {
        if (-not (Test-Path -LiteralPath $pk3)) {
            continue
        }

        $zip = [System.IO.Compression.ZipFile]::OpenRead($pk3)
        try {
            $entry = $zip.Entries | Where-Object { $_.FullName -ieq $entryPath } | Select-Object -First 1
            if (-not $entry) {
                continue
            }

            $reader = [System.IO.StreamReader]::new($entry.Open())
            try {
                return $reader.ReadToEnd()
            }
            finally {
                $reader.Dispose()
            }
        }
        finally {
            $zip.Dispose()
        }
    }

    throw "Could not find $entryPath in Jedi Outcast asset PK3s"
}

foreach ($menuFile in $menuFiles) {
    $menuText = Read-MenuFromAssets -MenuFile $menuFile
    $marker = "`t`t// EXIT button in lower left corner"
    if (-not $menuText.Contains($marker)) {
        throw "Could not find insertion point in ui/$menuFile"
    }

    if ($menuText -notmatch "name\s+coopgamebutton") {
        $menuText = $menuText.Replace($marker, $coopBlock + $marker)
    }

    [System.IO.File]::WriteAllText((Join-Path $modUi $menuFile), $menuText, [System.Text.Encoding]::ASCII)
}

Copy-Item -LiteralPath (Join-Path $repoUi "menus.txt") -Destination (Join-Path $modUi "menus.txt") -Force
Copy-Item -LiteralPath (Join-Path $repoUi "coop.menu") -Destination (Join-Path $modUi "coop.menu") -Force

Write-Host "Generated COOP menu overlays in $modUi"
