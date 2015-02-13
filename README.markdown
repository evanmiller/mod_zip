mod_zip
=======
[![Build Status](https://travis-ci.org/evanmiller/mod_zip.svg?branch=master)](https://travis-ci.org/evanmiller/mod_zip)

mod_zip assembles ZIP archives dynamically. It can stream component files from
upstream servers with nginx's native proxying code, so that the process never
takes up more than a few KB of RAM at a time, even while assembling archives that
are (potentially) hundreds of megabytes.


Installation
------------

To install, compile nginx with the following option:

    --add-module=/path/to/this/directory

nginx 0.7.25 or later is required. If libiconv is present, support for the
"X-Archive-Charset" header is enabled; see below.


Usage
-----

The module is activated when the original response (presumably from an
upstream) includes the following HTTP header:

    X-Archive-Files: zip

It then scans the response body for a list of files. The syntax is a 
space-separated list of the file checksum (CRC-32), size (in bytes), location
(properly URL-encoded), and file name. One file per line.  The file location
corresponds to a location in your nginx.conf; the file can be on disk, from an
upstream, or from another module.  The file name can include a directory path,
and is what will be extracted from the ZIP file. Example:

    1034ab38 428    /foo.txt   My Document1.txt
    83e8110b 100339 /bar.txt   My Other Document1.txt

Files are retrieved and encoded in order. If a file cannot be found or the file
request returns any sort of error, the download is aborted.

The CRC-32 is optional. Put "-" if you don't know the CRC-32; note that in this
case mod_zip will disable support for the "Range" header.

To re-encode the filenames as UTF-8, add the following header to the upstream
response:

    X-Archive-Charset: [original charset name]

The original charset name should be something that iconv understands. (This feature
only works if iconv is present.)

If you set original charset as 'native':

    X-Archive-Charset: native;

filenames from file list are accepted as already in native charset and zip's
general purpose flag (11 bit), that indicates UTF-8 encoded names, won't be set.
So archivers will know it's native charset.

Sometimes there is problem converting UTF-8 names to native(CP866) charset that
causes popular archivers to fail to recognize them. And at the same time you want
data not to be lost so that smart archivers can use Unicode Path extra field.
You can provide you own, adapted representation of filename in native charset along
with original UTF-8 name in one string. You just need to add following header:

    X-Archive-Name-Sep: [separator];

So your file list should look like:

    <CRC-32> <size> <path> <native-filename><separator><utf8-filename>
    ...

then filename field will contatin 'native-filename' and Unicode Path extra field
will contatin 'utf8-filename'.

Tips
----

Tip: Add a header "Content-Disposition: attachment; filename=foobar.zip" in the
upstream response if you would like the client to name the file "foobar.zip"

Tip 2: To save bandwidth, add a "Last-Modified" header in the upstream response; 
mod_zip will then honor the "If-Range" header from clients.

Tip 3: To wipe the X-Archive-Files header from the response sent to the client,
use the headers_more module: http://wiki.nginx.org/NginxHttpHeadersMoreModule

Tip 4: Using mod_zip with SSL will not work if the backend response has
a Set-Cookie header. Wipe it with the headers_more module (see tip 3).

Tip 5: Using mod_zip with SSL may stuck sometimes (or even always reproduce on
same files list), you can create extra proxy server in nginx configs that
will remove X-Archive-Files header (see tip 3).

Questions/patches may be directed to Evan Miller, emmiller@gmail.com.
