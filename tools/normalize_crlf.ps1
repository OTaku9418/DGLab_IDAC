# Rewrite a text file so every line ends with CRLF.
#
# Why this exists: the shipped DGLab_IDAC.ini must be CRLF. The mod itself
# normalises an LF file on first load (see load_config), but a release package
# should not depend on that: the first run would rewrite a file the user just
# unpacked, and any tool that reads the INI without going through the mod
# (a config checker, an editor, a diff) would see it empty.
#
# The repo keeps the template as LF (that is what .gitattributes says for
# *.template), so the conversion happens here, at packaging time.
param(
    [Parameter(Mandatory = $true)][string]$Path
)

$text = [IO.File]::ReadAllText($Path)
$text = $text -replace "`r`n", "`n"
$text = $text -replace "`n", "`r`n"
[IO.File]::WriteAllText($Path, $text, (New-Object System.Text.UTF8Encoding($false)))
Write-Output ("  normalised to CRLF: " + $Path)
