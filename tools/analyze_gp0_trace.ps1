param(
    [Parameter(Mandatory = $true)]
    [string[]] $Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Convert-Signed11([uint32] $Value) {
    $masked = $Value -band 0x7FF
    if (($masked -band 0x400) -ne 0) {
        return [int] ($masked - 0x800)
    }
    return [int] $masked
}

function Add-Vertex(
    [hashtable] $Bucket,
    [uint32] $Word,
    [int] $OffsetX,
    [int] $OffsetY
) {
    $x = (Convert-Signed11 $Word) + $OffsetX
    $y = (Convert-Signed11 ($Word -shr 16)) + $OffsetY
    $Bucket.MinX = [Math]::Min($Bucket.MinX, $x)
    $Bucket.MaxX = [Math]::Max($Bucket.MaxX, $x)
    $Bucket.MinY = [Math]::Min($Bucket.MinY, $y)
    $Bucket.MaxY = [Math]::Max($Bucket.MaxY, $y)
    ++$Bucket.Vertices
}

function Read-Gp0Trace([string] $TracePath) {
    $stream = [IO.File]::OpenRead((Resolve-Path -LiteralPath $TracePath))
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $magic = $reader.ReadUInt32()
        if ($magic -ne 0x534D4750) {
            throw "Invalid GP0 trace magic in $TracePath"
        }
        $displayX = $reader.ReadUInt32()
        $displayY = $reader.ReadUInt32()
        $displayWidth = $reader.ReadUInt32()
        $displayHeight = $reader.ReadUInt32()
        $packetCount = $reader.ReadUInt32()

        $drawX0 = $displayX
        $drawY0 = $displayY
        $drawX1 = $displayX + $displayWidth - 1
        $drawY1 = $displayY + $displayHeight - 1
        $offsetX = [int] $displayX
        $offsetY = [int] $displayY
        $drawToDisplay = 1
        $states = @{}

        for ($packetIndex = 0; $packetIndex -lt $packetCount; ++$packetIndex) {
            $wordCount = $reader.ReadUInt32()
            $words = [uint32[]]::new($wordCount)
            for ($wordIndex = 0; $wordIndex -lt $wordCount; ++$wordIndex) {
                $words[$wordIndex] = $reader.ReadUInt32()
            }
            if ($wordCount -eq 0) {
                continue
            }

            $opcode = $words[0] -shr 24
            switch ($opcode) {
                0xE1 {
                    $drawToDisplay = ($words[0] -shr 10) -band 1
                }
                0xE3 {
                    $drawX0 = $words[0] -band 0x3FF
                    $drawY0 = ($words[0] -shr 10) -band 0x1FF
                }
                0xE4 {
                    $drawX1 = $words[0] -band 0x3FF
                    $drawY1 = ($words[0] -shr 10) -band 0x1FF
                }
                0xE5 {
                    $offsetX = Convert-Signed11 $words[0]
                    $offsetY = Convert-Signed11 ($words[0] -shr 11)
                }
            }
            if ($opcode -lt 0x20 -or $opcode -gt 0x7F) {
                continue
            }

            $key =
                "dfe=$drawToDisplay clip=$drawX0,$drawY0-$drawX1,$drawY1 " +
                "ofs=$offsetX,$offsetY"
            if (-not $states.ContainsKey($key)) {
                $states[$key] = @{
                    Primitives = 0
                    Vertices = 0
                    MinX = 99999
                    MaxX = -99999
                    MinY = 99999
                    MaxY = -99999
                }
            }
            $bucket = $states[$key]
            ++$bucket.Primitives

            if ($opcode -le 0x3F) {
                $vertexCount = if (($opcode -band 0x08) -ne 0) { 4 } else { 3 }
                $textured = ($opcode -band 0x04) -ne 0
                $gouraud = ($opcode -band 0x10) -ne 0
                $index = 1
                for ($vertex = 0; $vertex -lt $vertexCount; ++$vertex) {
                    if ($vertex -ne 0 -and $gouraud) {
                        ++$index
                    }
                    if ($index -lt $wordCount) {
                        Add-Vertex $bucket $words[$index] $offsetX $offsetY
                    }
                    ++$index
                    if ($textured) {
                        ++$index
                    }
                }
            } elseif ($opcode -ge 0x60 -and $wordCount -ge 2) {
                Add-Vertex $bucket $words[1] $offsetX $offsetY
            }
        }

        [pscustomobject] @{
            Path = $TracePath
            Display = "$displayX,$displayY ${displayWidth}x${displayHeight}"
            States = $states
        }
    } finally {
        $reader.Dispose()
    }
}

foreach ($tracePath in $Path) {
    $trace = Read-Gp0Trace $tracePath
    "FILE=$($trace.Path) DISPLAY=$($trace.Display)"
    $trace.States.GetEnumerator() |
        Sort-Object { $_.Value.Primitives } -Descending |
        ForEach-Object {
            $value = $_.Value
            "$($value.Primitives) primitives; $($value.Vertices) vertices; " +
                "range=$($value.MinX),$($value.MinY)-" +
                "$($value.MaxX),$($value.MaxY); $($_.Key)"
        }
}
