param(
    [string]$FallbackVersion = "2.0.0"
)

$ErrorActionPreference = "Stop"

try {
    $tag = git tag --merged HEAD --sort=-version:refname |
        Where-Object { $_ -match '^v?\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$' } |
        Select-Object -First 1

    if ([string]::IsNullOrWhiteSpace($tag)) {
        $FallbackVersion
        exit 0
    }

    $version = $tag -replace '^v', ''
    $commitCount = [int](git rev-list "$tag..HEAD" --count)
    $shortHash = git rev-parse --short=8 HEAD
    $dirty = -not [string]::IsNullOrWhiteSpace(
        (git status --porcelain --untracked-files=no | Out-String)
    )

    if ($commitCount -gt 0 -or $dirty) {
        $parts = $version -split '\+', 2
        $version = $parts[0]
        $metadata = @()
        if ($parts.Count -eq 2) {
            $metadata += $parts[1]
        }
        if ($commitCount -gt 0) {
            $metadata += "$commitCount.g$shortHash"
        }
        if ($dirty) {
            $metadata += "dirty"
        }
        $version += "+" + ($metadata -join ".")
    }

    $version
} catch {
    $FallbackVersion
}
