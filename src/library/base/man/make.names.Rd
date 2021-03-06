% File src/library/base/man/make.names.Rd
% Part of the R package, http://www.R-project.org
% Copyright 1995-2008 R Core Team
% Modifications for pqR Copyright (c) 2016 Radford M. Neal.
% Distributed under GPL 2 or later

\name{make.names}
\alias{make.names}
\title{Make Syntactically Valid Names}
\description{
  Make syntactically valid names out of character vectors.
}
\usage{
make.names(names, unique = FALSE, allow_ = TRUE, 
    no.. = if (!getOption("parse_dotdot")) 0L else if (unique) 2L else 1L)
}
\arguments{
 \item{names}{character vector to be coerced to syntactically valid
   names.  This is coerced to character if necessary.}
 \item{unique}{logical; if \code{TRUE}, the resulting elements are
     unique.  This may be desired for, e.g., column names.}
 \item{allow_}{logical.  For compatibility with \R prior to 1.9.0.}
 \item{no..}{logical.  For compatibility with R Core versions that
                       allow \code{..} not at beginning or end}
}
\value{
  A character vector of same length as \code{names} with each changed to
  a syntactically valid name, in the current locale's encoding.
}
\details{
  A syntactically valid name contains only letters, numbers, and the dot or
  underline characters, starts with a letter, or with the dot not followed
  by a number, and does not contain a sequence of two or more consecutive dots 
  except at the beginning or end (unless the \code{parse_dotdot} option
  is \code{FALSE}).  Names such as \code{".2way"} are not valid
  and those such as \code{x..y} are not valid unless the \code{parse_dotdot}
  option is \code{FALSE}. The \link{reserved} words are also
  not valid.

  The definition of a \emph{letter} depends on the current locale, but
  only ASCII digits are considered to be digits.

  \code{make.names} translates missing values in \code{names} to \code{"NA"}.

  Otherwise, the process starts by prepending the character \code{X}
  if necessary.  All invalid characters are then translated to
  \code{.}.  Names which match \R keywords have a dot appended to
  them.  Finally, if \code{no..} is not 0, all sequences of
  consecutive dots that are not at the beginning are converted to a
  single dot, except that if \code{no..} is 1, this is not done for a
  sequence at the end.

  Finally, if \code{unique} is \code{TRUE}, duplicated values are altered by 
  \code{\link{make.unique}}.
}
\note{
  Prior to \R version 1.9.0, underscores were not valid in variable names,
  and code that relies on them being converted to dots will no longer
  work. Use \code{allow_ = FALSE} for back-compatibility.

  \code{allow_ = FALSE} is also useful when creating names for export to
  applications which do not allow underline in names (for example,
  S-PLUS and some DBMSs).
}
\seealso{
  \code{\link{make.unique}},
  \code{\link{names}},
  \code{\link{character}},
  \code{\link{options}},
  \code{\link{data.frame}}.
}
\examples{
make.names(c("a and b", "a-and-b"), unique=TRUE)
# "a.and.b"  "a.and.b.1"
make.names(c("a and b", "a_and_b"), unique=TRUE)
# "a.and.b"  "a_and_b"
make.names(c("a and b", "a_and_b"), unique=TRUE, allow_=FALSE)
# "a.and.b"  "a.and.b.1"

state.name[make.names(state.name) != state.name] # those 10 with a space
}
\keyword{character}
