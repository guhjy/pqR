% File src/library/base/man/DotDot.Rd
% Part of pqR.
% Copyright 2016 Radford M. Neal
% Distributed under GPL 2 or later

\name{DotDot}
\title{DotDot Operator}
\alias{..}
\alias{dotdot}
\description{
  Generate increasing sequences.
}
\usage{
from .. to
}
\arguments{
  \item{from}{starting value of sequence (numeric or integer).}
  \item{to}{(maximal) end value of the sequence (numeric or integer).}
}
\details{

  The primitive binary operator \code{..} generates an increasing sequence of
  values, starting at \code{from}, with each value in the sequence one
  greater than the previous value, ending when the next value would be
  greater than \code{to} (plus a small allowance for roundoff error of
  about 1e-7).  If \code{to} is less than \code{from} (minus about
  1e-7), the sequence will be of length zero.
  
  Non-numeric arguments are coerced internally (hence without
  dispatching methods) to numeric; complex values will have their
  imaginary parts discarded with a warning.

  The \code{..} operator is similar to \code{:}, except that it
  generates only increasing sequences, and therefore can generate a
  zero-length sequence.  Also, \code{..} has precedence just lower
  than arithmetic operators (and hence lower than \code{:}), so that,
  for example, \code{i..i+9} will generate a sequence of length 10, 
  whereas \code{i:i+9} results in a sequence of length one.

  The inconvenient precedence of \code{:}, and the fact it may generate a
  sequence in either direction depending on its arguments, are sources
  of bugs, which can be avoided by using \code{..}.  For example, if 
  \code{n} is zero, the loop 
\preformatted{    for (i in 1:n) X()
} 
  behaves in probably-unintended fashion, calling 
  \code{X()} twice, but the loop
\preformatted{    for (i in 1..n) X()
}
  will call \code{X()} zero times, as is probably the intent.

  Since \code{..} is allowed at the beginning or end of an identifier,
  uses of \code{..} as an operator will sometimes have to be surrounded 
  by spaces.  For example,
  \code{i..(i+n)} calls the function \code{i..} with argument \code{i+n};
  to use the \code{..} operator, this must be writen as \code{i .. (i+n)},
  or just as \code{i..i+n}.  However, since \code{..} is not allowed
  in the middle of an identifier, and identifiers are not permitted in some
  syntactic contexts, most uses of \code{..} do not require spaces.
  For example, in both \code{i..j} and \code{f()..end} the \code{..}
  is the \code{..} operator, not part of an identifier.

  Note: The parsing of \code{..} operator will be disabled if the
  \code{parse_dotdot} option is \code{FALSE} (though the primitive will
  still exist, and be accessible by calling it as a function named
  \code{`..`}).  
}

\value{
  A vector of type \code{\link{integer}} if \code{from} has a value
  that can be represented as an integer, and so can the other values
  in the sequence, and otherwise of type \code{double} (aka
  \code{\link{mode}} \code{"\link{numeric}"}).  

  The vector returned will have a \code{dim} attribute specifying that
  it is a one-dimensional array, which allows the result of an
  expression such as \code{1..n} when \code{n} is 1 to be
  distinguished from the scalar 1.
}
\seealso{
  \code{\link{:}}, \code{\link{seq}}, \code{\link{seq_len}},
  \code{\link{seq_along}}, \code{\link{options}}.
}
\examples{
options(parse_dotdot=TRUE)
11..14 # length 4
11..0  # length 0
pi..6  # real
2..pi  # integer
}
\keyword{manip}
