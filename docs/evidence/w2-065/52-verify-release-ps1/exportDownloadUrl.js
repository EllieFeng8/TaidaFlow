var Td = { pageHost: '192.168.0.125' };
function exportDownloadUrl(entry) {
        var url = entry && entry.url ? String(entry.url) : ""
        if (url.length === 0)
            return ""
        if (url.charAt(0) === "/") {
            var port = Number(entry.downloadPort) > 0 ? Number(entry.downloadPort) : 8124
            return "http://" + Td.pageHost + ":" + port + url
        }
        return url
    }
console.log(exportDownloadUrl({ url: '/exports/w2062verify_20260928_232805.csv', downloadPort: 80 }));