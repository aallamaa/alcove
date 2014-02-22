
===================================================
ALCOVE A YET TO DEFINE NEW KIND OF DATABASE/LANGAGE
===================================================

(PS: The following text will become a blog post and leave the place to a real README when this project will be advanced enough, this is really an alpha alpha status version of the project... )

History
=======


More than 20 years ago from now, when I was still a teenage coding on 8 bits and 16 bits processors using assembly languages, I came up with the idea of designing a new breed of processor. And this idea primarely came while working with the blitter and copper co-processors of the Amiga. The idea was the following, building processors that would be made of stacked alveolus, each hexagonal prism constituting this alveolus structure being able to exchange streams of data and code with its neighboring cells. I called this technology, X.One Technologies (from Hexagon and Axone, X.One => Multiple One).

Today, the world is being shaped by the Internet and this same principle applies. Each entity is becoming part of the Internet, and is exchanging with the rest of the Internet. Massive amounts of data are being processed, and the more we move forward, the more we need to be able to process quickly and efficiently those massive amounts.

Code and data are converging in a highly concurrent world, both at the processor level (increasing number of cores) and at the network level (distributed over network nodes).

What will ALCOVE be ?
=====================

When programming we always try to get as close as possible to the data through various abstraction layers. More and more database/storage technologies are greatly improving the developping experience (schema-less databases like mongodb, in memory nosql databases such as Redis).

But I feel we need code to be even closer to the data, so that's what this project is about. Getting something similar to Redis (In-memory key-value store) with some concurrency features (agent, generators,..). Basicaly data can be code, and code can be data, so when fetching data from a key for instance, you could potentially be triggering a piece of code which would return data without even knowing it.

The first part of this project is creating an interpretor for the langage which will be the core of the database. The homoiconicity of LISP made it the default choice, plus I never had the chance when i was young, student, or even when working to code in LISP so LISP (and also SmallTalk) is for me like the lost atlantide of computer science.

The main inspirations for this LISP are ARC (from Paul Graham) and Clojure. I will try to stick as much as possible to the ARC specs.



Install and Run
===============

make
./parser

I would advice to use rlwrap

rlwrap ./parser
