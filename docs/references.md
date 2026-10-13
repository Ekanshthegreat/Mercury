# References

Every source below was located during the literature review for this project and
checked against the publisher / primary listing at the time of writing. Notes
explain _why the source matters to Mercury_, not just what it is. If a claim in
Mercury's docs depends on a source, that source should be here.

> Verification note: we distinguish **primary sources** (the paper itself,
> publisher page, or the authors' own listing) from secondary descriptions. We
> cite primary sources where possible. Where we only had a secondary copy, this
> is stated.

---

## Limit-order-book models

**Huang, W., Lehalle, C.-A., & Rosenbaum, M. (2015). "Simulating and analyzing
order book data: The queue-reactive model." _Journal of the American
Statistical Association_, 110(509), 107–122.**
The core modelling idea Mercury borrows: event intensities (limit / cancel /
market) on a queue depend on the current state of that queue, chiefly the
size imbalance. This is the reason Mercury's order flow is _state-dependent_
rather than i.i.d. The authors' own project page describes the model and its
rationale: <http://www.cmap.polytechnique.fr/~charles-albert.lehalle/projects/2024QR/>

**Cont, R., Stoikov, S., & Talreja, R. (2010). "A stochastic model for order
book dynamics." _Operations Research_, 58(3), 549–563.**
The canonical tractable Markovian LOB model. Justifies modelling the book's
state with a small number of quantities (best quotes, queue sizes, spread) and
treating events as state-dependent Poisson arrivals. Used as a design
reference, and referenced in the LOB-Bench line of work as the "baseline"
generative model.
Publisher listing: <https://pubsonline.informs.org/doi/10.1287/opre.1090.0795>

**Huang, W., Lehalle, C.-A., & Rosenbaum, M. (2019). "Queue-reactive Hawkes
models for the order flow."** <https://ideas.repec.org/p/arx/papers/1901.08938.html>
Extension of the queue-reactive model with a Hawkes (self-exciting) component.
Motivates Mercury's optional self-excitation term: pure queue-reactive models
reproduce book shape well but tend to under-produce volatility; the Hawkes
term improves inter-event-time statistics.

**Abergel, F., Anane, M., Chakraborti, A., Jedidi, A., & Toke, I. M. (2024).
"A Novel Approach to Queue-Reactive Models: The Importance of Order Sizes."
arXiv:2405.18594.** <https://arxiv.org/abs/2405.18594>
Shows that modelling the _size_ of arriving orders as state-dependent (not just
their type and intensity) substantially improves agreement with stylized facts.
Supports Mercury treating size distributions as calibration targets.

---

## Order-flow self-excitation (Hawkes)

**Bacry, E., & Muzy, J.-F. (2014/2015). "Hawkes model for price and trades
high-frequency dynamics." arXiv:1301.1135.**
<https://arxiv.org/abs/1301.1135>
Multivariate Hawkes representation of trades and price changes, including the
concave / square-root market-impact profile. The reference for how signed
order-flow autocorrelation and impact are linked.

**Hawkes, A. G. (1971). "Spectra of some self-exciting and mutually exciting
point processes." _Biometrika_, 58(1), 83–90.**
The origin of the self-exciting process Mercury uses as an optional component.
Background reference; the exponential-kernel form is standard.
Overview with the multivariate and density-dependent extensions:
<https://en.wikipedia.org/wiki/Hawkes_process> (secondary).

---

## Optimal execution and market impact

**Almgren, R., & Chriss, N. (2000). "Optimal execution of portfolio
transactions." _Journal of Risk_, 3(2), 5–39.**
The foundational mean–variance execution model: trade off market-impact cost
(too fast) against timing risk (too slow). Mercury implements an
Almgren–Chriss-inspired _schedule_ as an optional baseline policy, and its
risk/cost trade-off framing underpins Mercury's insistence on tail risk.
Publisher: <https://www.risk.net/journal-risk/2161150/optimal-execution-portfolio-transactions>

**Bouchaud, J.-P. (2010). "The subtle nature of financial random walks."
Also the square-root-law literature.**
Verified via the 2025 synthesis papers below rather than a single in-hand
copy; the square-root market-impact law is the empirical target Mercury checks
qualitatively (impact grows sub-linearly in executed volume).

**Maitrier, G., et al. (2025). "Generating realistic metaorders from public
data." arXiv:2503.18199.** <https://arxiv.org/abs/2503.18199>
Reviews the Square Root Law of market impact and its universality, and
discusses generating metaorders from public data — directly relevant to
Mercury's calibration section and to the ethics of using only public data.

**Bouchaud, Bonart, Donier, & Gould (2018). _Trades, Quotes and Prices:
Financial Markets Under the Microscope._ Cambridge University Press.**
Standard reference for market impact, latency, and microstructure stylized
facts. Cited as a review; the individual claims we use (fat tails, volatility
clustering, order-sign long memory) are checked against it.

---

## Limit-order-book simulation reviews and benchmarks

**Prata, M., et al. (2024). "LOB-Bench: Benchmarking Generative AI for Finance
— an Application to Limit Order Book Data." arXiv:2502.09172.**
<https://arxiv.org/abs/2502.09172>
Provides a _benchmark_ framing for LOB generators, including Wasserstein-based
distributional distances and response-function scores, and benchmarks classical
models (Cont et al. 2010, Coletta et al.) against generative ones. Mercury's
validation suite deliberately mirrors these classical distances rather than
claiming a generative model.

**"Limit Order Book Simulations: A Review." (2024). arXiv:2402.17359.**
<https://arxiv.org/abs/2402.17359>
Taxonomy of LOB simulators (point processes / agent-based / deep learning /
SDEs) and a curated list of stylized facts. Used to select which stylized
facts Mercury validates against and to justify _not_ building an ABM in v1.

---

## Adversarial / robust stress testing

**Gretton, A., et al. (2012). "A Kernel Two-Sample Test." _JMLR_ 13.**
Background for distributional comparison (MMD); relevant if we add a two-sample
test between simulated and reference distributions.

**Robust optimisation / distributional robustness (Ben-Tal, El Ghaoui, &
Nemirovski; Bertsimas et al.).** Background for formulating adversarial regime
search as optimisation over a _constrained_ uncertainty set. Mercury's
plausibility constraints play the role of the uncertainty set in these
frameworks; we cite the concept rather than a specific single paper.

**European Central Bank stress-testing literature (e.g. ECB Occasional Paper
226, 2019).** <https://www.ecb.europa.eu/pub/pdf/scpops/ecb.op226~5e126a8e37.en.pdf>
Used only as methodological precedent for the _idea_ of adverse-but-plausible
scenarios and the requirement that scenarios be defensible. Not a
microstructure source.

---

## Computational methods

**Giles, M. B.** on multilevel Monte Carlo and **Glasserman, P. (2003).
_Monte Carlo Methods in Financial Engineering._** Background for variance
reduction; Mercury uses **common random numbers** for policy comparison, a
standard technique described in Glasserman.

**Metropolis, N., et al. (1953)** and modern **CMA-ES** (Hansen & Ostermeier, 2001) for derivative-free search. Mercury's evolutionary search follows the
CMA-ES family's black-box optimisation concept; we cite it as motivation for
the search method, not as a reproduction of it.

**Kirk, D. B., & Hwu, W. W. (2016). _Programming Massively Parallel
Processors._** Reference for CUDA memory-coalescing and occupancy reasoning
used in the GPU performance methodology.

---

## Data sources (public / licensed)

**Databento US Equities Mini** — <https://databento.com/catalog/us-equities>
A top-of-book US equities feed available without exchange license fees under
Databento's derived-data terms. Discussed in `data/README.md` as a _possible_
source users may license; Mercury does not redistribute it.

**LOBSTER** — <https://lobsterdata.com> Reconstructed NASDAQ (and other) full
depth-of-book message data. Widely used in academic microstructure work; users
may obtain it themselves. Mercury ships **no** LOBSTER data.

**Crypto LOB datasets (e.g. Crypto Lake, exchange public feeds).** Public
exchange websocket data (e.g. the recording tool in the `crypto-rl` project,
<https://github.com/sadighian/crypto-rl>) can be captured legally by the user.
Mercury's loader is format-agnostic; see `data/README.md`.

---

_If you spot a citation here that is wrong, the priority is to fix the
citation, not to keep the claim._
