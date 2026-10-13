# Literature background

These sources motivate the subject and modeling approach. Mercury is a simplified
engineering implementation; it does not reproduce the calibrated models or
optimal policies in these papers.

- **Huang, Weibing; Lehalle, Charles-Albert; Rosenbaum, Mathieu.**
  *Simulating and analyzing order book data: The queue-reactive model.*
  JASA 110(509), 107–122 (2015).
  [Author preprint](https://arxiv.org/abs/1312.0563).
  Background for relating event intensities to order-book state. Mercury uses
  its own simplified imbalance/spread functions and has no empirical calibration.
- **Cont, Rama; Stoikov, Sasha; Talreja, Rishi.**
  *A Stochastic Model for Order Book Dynamics.* Operations Research 58(3),
  549–563 (2010). DOI: 10.1287/opre.1090.0780.
  [Author paper](https://rama.cont.perso.math.cnrs.fr/pdf/CST2010.pdf),
  [institutional publication record](https://www.maths.ox.ac.uk/node/29748).
  Background for event-driven stochastic order books; Mercury is not a
  replication of the paper's analytical model.
- **Almgren, Robert; Chriss, Neil.**
  *Optimal execution of portfolio transactions.* Journal of Risk 3(2), 5–39
  (Winter 2000/2001). DOI: 10.21314/JOR.2001.041.
  [Publisher record](https://www.risk.net/journal-risk/2161150/optimal-execution-portfolio-transactions).
  Background for the cost-versus-timing-risk execution problem. Mercury's TWAP
  and adaptive heuristic are not an Almgren–Chriss optimizer.

The MVP does not implement the stylized-fact validation, data loaders, evolutionary
search, impact-law validation or CUDA methods envisioned in the original broader
plan. Their literature is not presented as evidence of implemented functionality.
