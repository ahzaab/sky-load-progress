[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$SourceLoadingMenu,

    [Parameter(Mandatory)]
    [string]$FfdecJar,

    [Parameter(Mandatory)]
    [string]$JavaExe
)

$ErrorActionPreference = 'Stop'

$projectDirectory = Split-Path -Parent $PSScriptRoot
$workingDirectory = Join-Path $projectDirectory 'build\meter-asset'
$sourceXml = Join-Path $workingDirectory 'loadingmenu.xml'
$meterXml = Join-Path $workingDirectory 'LoadingProgressMeter.xml'
$meterSwf = Join-Path $workingDirectory 'LoadingProgressMeter.swf'

New-Item -ItemType Directory -Path $workingDirectory -Force | Out-Null

& $JavaExe -jar $FfdecJar -swf2xml $SourceLoadingMenu $sourceXml
if ($LASTEXITCODE -ne 0) {
    throw 'FFDec could not export loadingmenu.swf.'
}

[xml]$document = Get-Content -Raw -LiteralPath $sourceXml
$document.swf.frameCount = '1'
$tags = $document.swf.tags
$controlTags = @(
    'DoActionTag',
    'FrameLabelTag',
    'PlaceObject2Tag',
    'PlaceObject3Tag',
    'RemoveObjectTag',
    'RemoveObject2Tag',
    'ShowFrameTag'
)

# Remove only root timeline controls. Sprite timelines remain untouched.
foreach ($item in @($tags.item)) {
    if ($controlTags -contains $item.type) {
        [void]$tags.RemoveChild($item)
    }
}

# Character 14 contains both the static frame and the animated fill. Remove the frame so the
# animation can scale horizontally without deforming the frame artwork.
$meterSprite = @($tags.item) | Where-Object { $_.type -eq 'DefineSpriteTag' -and $_.spriteId -eq '14' }
$embeddedFrame = @($meterSprite.subTags.item) | Where-Object {
    $_.type -eq 'PlaceObject2Tag' -and $_.characterId -eq '5' -and $_.depth -eq '1'
}
if ($meterSprite.Count -ne 1 -or $embeddedFrame.Count -ne 1) {
    throw 'Could not separate the vanilla meter frame from its animation.'
}
[void]$meterSprite.subTags.RemoveChild($embeddedFrame)

# Character 5 is the meter's static frame. Wrap it so Scaleform can apply a scaling grid to the
# frame independently of the animated fill and mask.
$frameSprite = $document.CreateElement('item')
$frameSprite.SetAttribute('type', 'DefineSpriteTag')
$frameSprite.SetAttribute('forceWriteAsLong', 'true')
$frameSprite.SetAttribute('frameCount', '1')
$frameSprite.SetAttribute('hasEndTag', 'true')
$frameSprite.SetAttribute('spriteId', '25')
$frameSubTags = $document.CreateElement('subTags')

$placeBoundsShape = $document.CreateElement('item')
$placeBoundsShape.SetAttribute('type', 'PlaceObject2Tag')
$placeBoundsShape.SetAttribute('characterId', '5')
$placeBoundsShape.SetAttribute('clipDepth', '0')
$placeBoundsShape.SetAttribute('depth', '1')
$placeBoundsShape.SetAttribute('forceWriteAsLong', 'false')
$placeBoundsShape.SetAttribute('placeFlagHasCharacter', 'true')
$placeBoundsShape.SetAttribute('placeFlagHasClipActions', 'false')
$placeBoundsShape.SetAttribute('placeFlagHasClipDepth', 'false')
$placeBoundsShape.SetAttribute('placeFlagHasColorTransform', 'false')
$placeBoundsShape.SetAttribute('placeFlagHasMatrix', 'true')
$placeBoundsShape.SetAttribute('placeFlagHasName', 'false')
$placeBoundsShape.SetAttribute('placeFlagHasRatio', 'false')
$placeBoundsShape.SetAttribute('placeFlagMove', 'false')
$placeBoundsShape.SetAttribute('ratio', '0')

$boundsShapeMatrix = $document.CreateElement('matrix')
$boundsShapeMatrix.SetAttribute('type', 'MATRIX')
$boundsShapeMatrix.SetAttribute('hasRotate', 'false')
$boundsShapeMatrix.SetAttribute('hasScale', 'false')
$boundsShapeMatrix.SetAttribute('nRotateBits', '0')
$boundsShapeMatrix.SetAttribute('nScaleBits', '0')
$boundsShapeMatrix.SetAttribute('nTranslateBits', '0')
$boundsShapeMatrix.SetAttribute('rotateSkew0', '0.0')
$boundsShapeMatrix.SetAttribute('rotateSkew1', '0.0')
$boundsShapeMatrix.SetAttribute('scaleX', '0.0')
$boundsShapeMatrix.SetAttribute('scaleY', '0.0')
$boundsShapeMatrix.SetAttribute('translateX', '0')
$boundsShapeMatrix.SetAttribute('translateY', '0')
[void]$placeBoundsShape.AppendChild($boundsShapeMatrix)
[void]$frameSubTags.AppendChild($placeBoundsShape)

$frameShowFrame = $document.CreateElement('item')
$frameShowFrame.SetAttribute('type', 'ShowFrameTag')
$frameShowFrame.SetAttribute('forceWriteAsLong', 'false')
[void]$frameSubTags.AppendChild($frameShowFrame)
[void]$frameSprite.AppendChild($frameSubTags)
[void]$tags.AppendChild($frameSprite)

# Bounds_mc uses a separate sprite definition so its invisible layout transform cannot affect
# Frame_mc. Both definitions reference the same skin artwork.
$boundsSprite = $frameSprite.CloneNode($true)
$boundsSprite.SetAttribute('spriteId', '26')
[void]$tags.AppendChild($boundsSprite)

# Preserve roughly 20 pixels of frame artwork at each end; only the long center section may stretch.
$meterScalingGrid = $document.CreateElement('item')
$meterScalingGrid.SetAttribute('type', 'DefineScalingGridTag')
$meterScalingGrid.SetAttribute('characterId', '25')
$meterScalingGrid.SetAttribute('forceWriteAsLong', 'false')

$meterSplitter = $document.CreateElement('splitter')
$meterSplitter.SetAttribute('type', 'RECT')
$meterSplitter.SetAttribute('Xmin', '-1800')
$meterSplitter.SetAttribute('Xmax', '1800')
$meterSplitter.SetAttribute('Ymin', '-100')
$meterSplitter.SetAttribute('Ymax', '100')
$meterSplitter.SetAttribute('nbits', '13')
[void]$meterScalingGrid.AppendChild($meterSplitter)
[void]$tags.AppendChild($meterScalingGrid)

$placeMeter = $document.CreateElement('item')
$placeMeter.SetAttribute('type', 'PlaceObject2Tag')
$placeMeter.SetAttribute('characterId', '14')
$placeMeter.SetAttribute('clipDepth', '0')
$placeMeter.SetAttribute('depth', '2')
$placeMeter.SetAttribute('forceWriteAsLong', 'true')
$placeMeter.SetAttribute('name', 'Meter_mc')
$placeMeter.SetAttribute('placeFlagHasCharacter', 'true')
$placeMeter.SetAttribute('placeFlagHasClipActions', 'false')
$placeMeter.SetAttribute('placeFlagHasClipDepth', 'false')
$placeMeter.SetAttribute('placeFlagHasColorTransform', 'false')
$placeMeter.SetAttribute('placeFlagHasMatrix', 'true')
$placeMeter.SetAttribute('placeFlagHasName', 'true')
$placeMeter.SetAttribute('placeFlagHasRatio', 'false')
$placeMeter.SetAttribute('placeFlagMove', 'false')
$placeMeter.SetAttribute('ratio', '0')

$matrix = $document.CreateElement('matrix')
$matrix.SetAttribute('type', 'MATRIX')
$matrix.SetAttribute('hasRotate', 'false')
$matrix.SetAttribute('hasScale', 'false')
$matrix.SetAttribute('nRotateBits', '0')
$matrix.SetAttribute('nScaleBits', '0')
$matrix.SetAttribute('nTranslateBits', '0')
$matrix.SetAttribute('rotateSkew0', '0.0')
$matrix.SetAttribute('rotateSkew1', '0.0')
$matrix.SetAttribute('scaleX', '0.0')
$matrix.SetAttribute('scaleY', '0.0')
$matrix.SetAttribute('translateX', '0')
$matrix.SetAttribute('translateY', '0')
[void]$placeMeter.AppendChild($matrix)
$placeFrame = $placeMeter.CloneNode($true)
$placeFrame.SetAttribute('characterId', '25')
$placeFrame.SetAttribute('depth', '1')
$placeFrame.SetAttribute('name', 'Frame_mc')
[void]$tags.AppendChild($placeFrame)
[void]$tags.AppendChild($placeMeter)

$placeBounds = $placeMeter.CloneNode($true)
$placeBounds.SetAttribute('characterId', '26')
$placeBounds.SetAttribute('depth', '3')
$placeBounds.SetAttribute('name', 'Bounds_mc')
$placeBounds.SetAttribute('placeFlagHasColorTransform', 'true')
$colorTransform = $document.CreateElement('colorTransform')
$colorTransform.SetAttribute('type', 'CXFORMWITHALPHA')
$colorTransform.SetAttribute('alphaAddTerm', '0')
$colorTransform.SetAttribute('alphaMultTerm', '0')
$colorTransform.SetAttribute('blueAddTerm', '0')
$colorTransform.SetAttribute('blueMultTerm', '256')
$colorTransform.SetAttribute('greenAddTerm', '0')
$colorTransform.SetAttribute('greenMultTerm', '256')
$colorTransform.SetAttribute('hasAddTerms', 'false')
$colorTransform.SetAttribute('hasMultTerms', 'true')
$colorTransform.SetAttribute('nbits', '10')
$colorTransform.SetAttribute('redAddTerm', '0')
$colorTransform.SetAttribute('redMultTerm', '256')
[void]$placeBounds.AppendChild($colorTransform)
[void]$tags.AppendChild($placeBounds)

$showFrame = $document.CreateElement('item')
$showFrame.SetAttribute('type', 'ShowFrameTag')
$showFrame.SetAttribute('forceWriteAsLong', 'false')
[void]$tags.AppendChild($showFrame)

$writerSettings = [System.Xml.XmlWriterSettings]::new()
$writerSettings.Encoding = [System.Text.UTF8Encoding]::new($false)
$writerSettings.Indent = $true
$writer = [System.Xml.XmlWriter]::Create($meterXml, $writerSettings)
try {
    $document.Save($writer)
} finally {
    $writer.Dispose()
}

Remove-Item -LiteralPath $meterSwf -Force -ErrorAction SilentlyContinue
& $JavaExe -jar $FfdecJar -xml2swf $meterXml $meterSwf
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $meterSwf -PathType Leaf)) {
    throw 'FFDec could not build LoadingProgressMeter.swf.'
}

$destinations = @(
    (Join-Path $projectDirectory 'dist\Interface\SkyrimLoadProgress'),
    (Join-Path $projectDirectory 'dist\Interface\Exported\SkyrimLoadProgress')
)

foreach ($destination in $destinations) {
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath $meterSwf -Destination $destination -Force
}

Write-Host "Built standalone loading meter: $meterSwf"
