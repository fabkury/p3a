# Q&A preparation

Anticipated questions from an IIIF audience, with short answers grounded
in the measurements in `material.md`. Fifteen minutes of questions is
roughly six to eight exchanges. The first three below are the ones most
likely to come from this crowd.

## On the discovery-gap conclusion

**1. Presentation 3.0 says discovery is out of scope. Isn't your
critique aimed at the wrong spec?**
Agreed, and that is the conclusion of the talk: the split p3a ships is
the division of labor the specs prescribe. The observation is not that
Presentation failed at discovery; it is that nothing in the IIIF family
serves a client that needs "artworks with images in collection X, how
many, give me 400-499" without building its own index. Change Discovery
presumes a harvester. Content Search is within one object. The question
for the room is whether that is a deliberate boundary or a gap worth a
profile or a new small spec.

**2. Change Discovery plus a local index is the intended model. Why not
run the harvester somewhere else and let the device query it?**
That is a valid architecture and would work. It also reintroduces a
cloud middleman, which p3a deliberately does not have: no account, no
server of mine between the museum and the frame. Every museum already
runs the index; the bespoke search APIs are that index. What I would
love is a thin standard query surface over it, not a copy.

**3. Wellcome and Harvard were down when you probed. Isn't "adoption is
broken" just two outages?**
Both were probed on two dates two days apart, Harvard with and without a
valid key, and Wellcome's responses self-described as temporary on both
dates. I will re-probe the week before this call and say what I find.
But the adoption ground is the weakest of the three on purpose: even
with perfect uptime, the spec has no counts, no offsets, no paging, and
the economics are 50-100x more requests. Fix the outages and the
conclusion does not move.

**4. Presentation 2.1 had paging and totals. Would 2.1 Collections have
worked?**
Closer. `total` is optional and navigation is link-following, so it
gives sequential traversal with an optional count: exactly the
Rijksmuseum cursor walk, whose costs the talk shows. No offset access,
no facets, and nobody among the nine serves a paged 2.1 Collection.

**5. Have you looked at the IIIF Discovery community group's work, or at
proposals for a "Collection Query" or search over Collections?**
I have read the published specs and the discuss.iiif.io archive; I have
not followed the working group in real time. If there is an active
proposal, I want to read it and would happily test it against a client
this small. That is the best outcome of this call.

## On the client design

**6. Why skip `info.json`? It is the whole point of the Image API.**
Panel is 720x720, `!720,720` was honored by every server, and skipping
halves the request count on a device that pays a TLS round trip per
request. Zero misfires since May. Where advertisement and reality
diverged (SMK's WebP), negotiation would have made things worse, not
better. I would add it the day a museum serves something other than a
best-fit JPEG for that request.

**7. Why Image API 2 and not 3?**
Every server accepted the v2 URL shape and several only advertise v2.
The URL template is the same for the size request I make. Nothing in
the client depends on the version beyond the path.

**8. Why not WebP? It would halve the bytes.**
Museum servers serve JPEG far more reliably; SMK advertises WebP and
returns 400. And the P4 has a hardware JPEG decoder; WebP would be
software only.

**9. Why not store metadata on the device?**
64 bytes per artwork, 48 of them for the image id, 4,096 artworks per
channel, 64 channels. Metadata is fetched by the browser on demand when
the viewer taps the info button, straight from the museum's API. It
also keeps the device honest as a measuring instrument: the comparison
between museums reduces to three primitives.

**10. Two TLS sessions and 32 KB chunks: is that the C6 co-processor or
the P4?**
The P4 does TLS with mbedTLS; the C6 is only the radio, reached over
SDIO through esp_hosted. Two sessions is a RAM budget: each TLS context
costs tens of kilobytes of internal SRAM, and the display pipeline needs
the rest.

**11. How do you handle attribution and rights on the device?**
The device shows images full-bleed with no overlay. Attribution, title,
artist, and date are one tap away in the web app, fetched live from the
museum. Sources are scoped to open content where the API allows it:
Cleveland CC0 only, Minneapolis public domain only, Harvard
`imagepermissionlevel:0`, AIC public-domain images. Smithsonian's CC0
filter is not indexed, so that one relies on the Open Access dataset's
own scope.

## On museums

**12. You named our museum. Can we fix the thing you found?**
Yes, and thank you. Everything in the table was observed from outside
with an identifying User-Agent; the probes are in the repo and I will
send the exact requests. The one-line fixes: an HTTP error instead of a
200 for rejected requests, `Retry-After` on 429s, and an `info.json`
that matches the server.

**13. Why is AIC disabled?**
Since August its IIIF host answers non-browser clients with a Cloudflare
managed challenge; the metadata API is fine. The device's TLS
fingerprint is the thing being refused. AIC was the first museum I
integrated and has the best-documented API I found; cached artwork
still plays, and the adapter is one flag away from coming back. Bot
defense is a compatibility issue now, and honest identification should
stay a sufficient answer.

**14. Why not Europeana or DPLA? One adapter, thousands of providers.**
Europeana's Thumbnail API caps at 400 px, and provider-level image URLs
are too heterogeneous for one adapter; DPLA serves thumbnails only. If
someone knows a reliable path to mid-size renditions across Europeana
providers, I asked on IIIF-Discuss and I am still asking.

**15. Would you add [our museum]?**
If the API lists artworks with counts at an offset and carries an image
identifier inline, and images arrive via IIIF or a rendition near 720
px, it is a few hundred lines of C and a JS adapter. Talk to me after.
Getty and Yale are the ones I would most like to add and cannot yet,
because both discover only via ActivityStreams or LUX.

**16. Library of Congress has a great IIIF service. Why not?**
Identifier length. LoC IIIF ids run 27-47 characters in Prints and
Photographs and maps, and 67-123 in newspapers and music, against a
48-byte slot. And many listings carry only a 150 px thumbnail with no
way to tell whether the IIIF service exists for the item. It is the
clearest case of identifier length being an interoperability property.

## On the project

**17. Is this commercial? Who pays for it?**
No. Apache 2.0, no product, no company, no sponsor. Board is $39.99
retail from Waveshare; I sell nothing. Makapix Club, the pixel-art
community it started with, is also noncommercial open source.

**18. How much of this did the LLM write?**
Most of the code, under my direction, with the design done as Q&A
transcripts that live in the repo. I verified every technical claim in
the talk and in the article, and I take responsibility for them. The
part I want on the record is the origin: an agent looking for content
sources came back with the IIIF Image API, which I had never heard of.

**19. Is this really the first firmware-level IIIF client?**
As far as I can tell. I searched discuss.iiif.io, the Consortium's news
archive, the Code4Lib Journal back catalog, and the open web, and asked
on the list. If someone here knows of one, I want to cite it.

**20. Can I read the longer write-up?**
A full article with all the measurements is under review at the
Code4Lib Journal. The measurements themselves are in the repository
under `docs/outreach/code4lib-journal/` and `docs/art-institutions/`.

**21. Will the slides be available?**
Yes, PDF, CC BY 4.0, sent to the organizers after the call.

## Questions to ask the room if there is silence

- Does anyone serve a paged Collection with totals today, in any
  version?
- Is there an existing effort toward a query surface over Collections,
  or a profile for small clients?
- For those running IIIF behind a WAF or Cloudflare: what should a
  legitimate non-browser client send so it is let through?
